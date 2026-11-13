/*
 * libctru side of the audio-thread boundary. This file is its own
 * translation unit and must never include game headers -- see
 * n3ds_backend.h for why (same collision, same isolation style). The
 * lock-scoping and core-selection choices below are not incidental; they
 * are the point of the design.
 *
 * This file also owns the ndsp output sink: a small ring of linear-memory
 * wave buffers the XA producer (the worker thread, or the game thread's
 * N3DS_AudioInlineStepVBlank when no worker exists) submits one vblank's
 * source-rate stereo PCM into. ndsp resamples it to the DSP's output rate.
 */

#include <3ds.h>
#include <string.h>
#include <stdio.h>

#include "n3ds_audio_thread.h"
#include "n3ds_backend.h" /* N3DS_DebugLog only -- plain-scalar boundary header, no game types */
#include "n3ds_beat.h"

/*
 * Defined in native_audio.c (the unity build's own translation unit).
 * NativeAudio_RenderFramesNoLock itself is `internal` (static, macros.h:74),
 * so it has no external symbol this file could link against, and this file
 * cannot include native_audio.c's own headers to share a prototype -- doing
 * so would pull the game's PSX headers into this libctru-visible TU, which
 * is exactly the collision n3ds_backend.h's isolation exists to avoid.
 * NativeAudio_RenderVBlankForAudioThread is a small external-linkage wrapper
 * native_audio.c exposes specifically for this cross-TU call: it owns its
 * own scratch buffer and hands the rendered samples to
 * N3DS_NdspSubmitMusicParcel below, so nothing PSX-typed needs to cross
 * this boundary -- the signature is deliberately void->void. There is no
 * shared header enforcing this prototype; if that wrapper's signature ever
 * changes, this declaration must be updated by hand.
 */
extern void NativeAudio_RenderVBlankForAudioThread(void);

/*
 * Defined in native_audio.c, same cross-TU reasoning as
 * NativeAudio_RenderVBlankForAudioThread above: NativeAudio_XAStreamPump is
 * `internal` (static, macros.h:74), scoped to that file's own translation
 * unit, so this file has no external symbol to link against without a
 * wrapper. Unlike the render wrapper, no PSX-typed buffer needs to cross
 * this boundary at all -- NativeAudio_XAStreamPump is already void->void,
 * so the wrapper is a pure pass-through with no scratch buffer of its own.
 *
 * Locking discipline this file's caller (N3DS_AudioThreadMain below) must
 * uphold: NativeAudio_XAStreamPump() internally takes
 * NativeAudio_LockOutput()/UnlockOutput(), which resolve on this platform
 * to s_audioRenderLock via N3DS_AudioLock/N3DS_AudioUnlock -- the SAME lock
 * the render call above takes. LightLock is non-recursive, so this must
 * only be called AFTER s_audioRenderLock has been released, never nested
 * inside that lock's held span, or the worker thread self-deadlocks trying
 * to re-lock a lock it already holds.
 */
extern void NativeAudio_XAPumpForAudioThread(void);

/*
 * Same hand-matched cross-TU contract as the two above. Renders one vblank
 * of the XA/CD music stream and submits it to the music channel; takes the
 * render lock itself, so it must be called OUTSIDE the drain loop's lock
 * span. Returns 1 if a parcel was submitted, 0 if XA was idle. The worker
 * paces it by wall-clock (svcGetSystemTick) below -- XA runs at real time
 * no matter how slowly the game emits vblanks, which is what the PS1's CD
 * hardware did.
 */
extern int NativeAudio_RenderXAParcelForAudioThread(void);

/*
 * Same hand-matched cross-TU contract. Finishes the chunked first-play
 * decode of any hardware sample-cache entry whose key-on only produced the
 * head chunk. Takes the render lock itself in short holds, so call it with
 * NO locks held, like the XA pump above.
 */
extern void NativeAudio_HwDecodePumpForAudioThread(void);

/*
 * Same hand-matched cross-TU contract. Drains native_audio.c's own half of
 * the worker profile (the lifecycle/mirror split inside the render call
 * above, which only that file can see) in ticks from N3DS_AudioTicksNow, and
 * zeroes it. Called from N3DS_AudioThreadGetProfile so the whole breakdown
 * is reported and reset as one unit.
 */
extern void NativeAudio_TakeWorkerProfileForAudioThread(unsigned long long *lifecycleTicks, unsigned long long *mirrorTicks);

/*
 * Reuses NATIVE_VSYNC_CATCHUP_MAX's existing value and precedent (the
 * render-side vblank catch-up loop, native_platform.c) -- same shape of
 * problem, same answer, not a new arbitrary number..
 */
#define NATIVE_AUDIO_THREAD_MAX_BACKLOG_VBLANKS 8

/*
 * Upper bound on one parcel: one vblank of stereo PCM at the highest rate a
 * producer can submit. Hand-matched to NATIVE_AUDIO_VBLANK_FRAMES
 * (native_audio.c: 44100/60 = 735) the same way the extern prototypes above
 * are hand-matched -- the game-side header cannot cross this boundary.
 *
 * A parcel is now "one vblank of audio", NOT "735 frames": this channel is
 * fed XA source PCM at its native 37800/18900 rate (630/315 frames
 * per vblank) so the DSP resamples in hardware and the CPU resampler --
 * measured at 52-56ms per 16.7ms parcel on the ARM11, i.e. the "talking into
 * a fan" announcer -- disappears entirely. 735 (one vblank at 44100) stays
 * the bound; the parcel unit keeps its meaning for the cushion target at any
 * source rate.
 *
 * N3DS_NdspSubmitMusicParcel rejects (and counts) anything outside
 * 1..MAX, so a drift is loud in the counters rather than silent corruption.
 */
#define N3DS_NDSP_PARCEL_FRAMES_MAX 735
/* The software mixer's fixed PSX SPU rate (native_audio.c
 * NATIVE_AUDIO_SAMPLE_RATE, hand-matched like the frame count above): the
 * channel's starting rate before any producer has submitted. */
#define N3DS_NDSP_MIXER_RATE_HZ     44100
/*
 * The sink accumulates N3DS_NDSP_AGG_PARCELS parcels into one wave buffer
 * before handing off to the DSP, to reduce resampler seam frequency (fewer,
 * larger buffers = fewer stitch points for the DSP's polyphase filter).
 * Costs up to ~133ms extra latency; a partial buffer flushes early when the
 * DSP is about to run dry so track starts aren't delayed a full aggregate.
 * Pool: N3DS_NDSP_BLOCK_COUNT aggregate buffers; full pool drops the newest
 * parcel (counted), never blocks the mixer.
 */
#define N3DS_NDSP_AGG_PARCELS  8
#define N3DS_NDSP_AGG_FRAMES   (N3DS_NDSP_PARCEL_FRAMES_MAX * N3DS_NDSP_AGG_PARCELS)
#define N3DS_NDSP_BLOCK_COUNT  6
/*
 * Aggregate handoff target, in parcels. N3DS_NDSP_AGG_PARCELS above is the slot
 * CAPACITY; this is the target. 3 hands off ~2.7x more often than 8, so the
 * DSP-side queue stays ~9-12 of the 12-parcel cushion instead of draining
 * toward 0 between handoffs -- which is what made XA voices/cutscenes pop
 * (hardware-tuned: 8 popped, 1 emptied the cushion and stalled the intro, 3
 * de-pops while keeping the full cushion).
 */
#define N3DS_NDSP_AGG_DEFAULT  3
/*
 * Channel plan (ndsp has exactly 24): channel 23 carries the music/parcel
 * stream (XA + anything else MixFrame still renders), leaving 0..22 for the
 * PS1's 24 SPU voices as MONO_PCM16 hardware voices. The voice->channel map
 * is dynamic, not 1:1: a voice holds a channel from key-on until it is
 * stopped or the DSP consumes its last wave buffer, and a key-on with no
 * channel of its own takes whichever idle channel has gone unused longest.
 * A fixed 1:1 map leaves one SPU voice permanently mute, and the game keys
 * every one of them on. Denials -- all 23 channels sounding at once -- are
 * counted (N3DS_HwVoiceGetDenied).
 */
#define N3DS_NDSP_CHANNEL      23
#define N3DS_HW_CHANNEL_COUNT  23
/*
 * The PS1's voice count, hand-matched to native_audio.c's
 * NATIVE_AUDIO_SPU_VOICE_COUNT the same way the parcel frame count above is:
 * the game-side header cannot cross this TU boundary.
 */
#define N3DS_SPU_VOICE_COUNT   24

/*
 * Output-stage gain, applied only where mix levels meet ndsp (the music
 * channel's SetMix and the hardware voices' SetMix) -- never inside the
 * mixer, whose PC byte-identity the SPU golden test enforces. Retail sets
 * master volume 0x3fff, PS1's max but only half of full digital scale (see
 * NATIVE_AUDIO_DIRECT_VOL_MAX, native_audio.c), so faithful amplitude is
 * quiet on the 3DS; 2.0 restores full scale. PS1 content peaks well below
 * full scale at that master, so 2.0 has headroom, and the DSP saturates
 * rather than wraps if a peak ever exceeds it. Runtime: dumpreq `vol N`
 * (percent of SPU-faithful, 200 = this default).
 */
#define N3DS_NDSP_OUTPUT_GAIN_DEFAULT 2.0f
static float s_ndspOutputGain = N3DS_NDSP_OUTPUT_GAIN_DEFAULT;

/* Protects s_audio (native_audio.c) during MixFrame -- backs
 * N3DS_AudioLock/N3DS_AudioUnlock, which back SDL_Lock/UnlockAudioStream. */
static LightLock s_audioRenderLock;
/* Protects ONLY the three counters below -- held for a handful of
 * instructions, never across rendering. */
static LightLock s_audioCounterLock;
/* Kernel event (RESET_STICKY), not a LightEvent: the worker's wait doubles
 * as the wall-clock XA pacer via svcWaitSynchronization's timeout, which
 * LightEvent cannot do. Signaled per vblank by the main thread and once at
 * shutdown; ~60 syscalls/s, nothing like the per-triangle scope tax. */
static Handle s_audioWakeHandle;
static u32 s_targetVBlanks;   /* written only by main thread, under s_audioCounterLock */
static u32 s_renderedVBlanks; /* written only by worker thread, under s_audioCounterLock */
static u32 s_droppedVBlanks;  /* diagnostic only, written only by main thread, under s_audioCounterLock */
static Thread s_audioThread;  /* NULL if never created (fallback path) or after join */
static volatile int s_audioThreadShouldStop;
static volatile int s_audioThreadRunning; /* set true only after a real thread on core 1 is confirmed running */

/*
 * ndsp sink state. Written only from N3DS_AudioThreadInit/Shutdown (main
 * thread) and N3DS_NdspSubmitMusicParcel. Submit runs on exactly one thread
 * at any point in the process lifetime -- the worker when one exists, else
 * the game thread through N3DS_AudioInlineStepVBlank (N3DS_AudioThreadIsRunning
 * never flips true after init) -- so the ring cursor needs no lock of its own.
 */
static int s_ndspInitAttempted;
static int s_ndspUp;
static int s_ndspForcedOff; /* dumpreq `ndspoff` latch: blocks N3DS_NdspSinkInit permanently */
static s16 *s_ndspPcm;                                  /* linearAlloc: BLOCK_COUNT x AGG_FRAMES x stereo */
static ndspWaveBuf s_ndspBufs[N3DS_NDSP_BLOCK_COUNT];   /* statuses start 0 = NDSP_WBUF_FREE */
static int s_ndspBufParcels[N3DS_NDSP_BLOCK_COUNT];     /* vblanks of audio in each handed-over buffer */
static int s_ndspNextBuf;
static int s_ndspAggParcels; /* parcels accumulated in s_ndspBufs[s_ndspNextBuf] so far (0..AGG_PARCELS) */
static int s_ndspAggFrames;  /* frames accumulated in that slot -- the append cursor, rate-agnostic */
static u32 s_ndspDroppedBlocks;
/* Channel state, so the pokes happen on change instead of per submit. Rate 0
 * = never set. See N3DS_NdspApplyChannelState. */
static int s_ndspRate;
static float s_ndspGainLeft = -1.0f;
static float s_ndspGainRight = -1.0f;
/* Which mode the sink actually ran in -- full 8-parcel aggregates (seams
 * ~7.5/sec, the fix engaged) vs early anti-underrun flushes (small buffers,
 * degenerates toward the old 60-crumb behavior). Azahar's HLE gulps the
 * queue and forces the early path constantly, so it CANNOT exercise the
 * batching; these counters are how a hardware log proves it engaged. */
static u32 s_ndspFullFlushes;
static u32 s_ndspEarlyFlushes;
/* Submits where the DSP-side queue was ALREADY empty when we went to append --
 * i.e. the channel had run dry and was outputting silence/stale until this
 * flush. This is the real playback-gap count, the XA-pop signal; aggEarly
 * counts the recovery flushes, this counts the gaps that forced them. */
static u32 s_ndspDspEmptyEvents;
/* Hardware-voice linear store (full contract at the plan-B section below);
 * declared here because N3DS_NdspSinkShutdown frees it. */
#define N3DS_HW_STORE_BYTES  (3 * 1024 * 1024)
#define N3DS_HW_STORE_BLOCKS 256
struct N3DS_HwStoreBlock
{
	u32 off;
	u32 bytes;
	int inUse;
};
static s16 *s_hwStore;         /* linearAlloc'd; NULL until first use */
static u32 s_hwStoreHiWater;   /* bytes the block table spans; the tail cursor never retreats */
static u32 s_hwStoreLive;      /* bytes held by allocated blocks right now */
/* Offset-ordered and contiguous over [0, s_hwStoreHiWater) -- both the
 * best-fit search and the coalesce on release depend on that order. */
static struct N3DS_HwStoreBlock s_hwStoreBlocks[N3DS_HW_STORE_BLOCKS];
static int s_hwStoreBlockCount;

/* Both reset state this file defines further down, next to the hardware
 * voices that own it. */
static void N3DS_HwStoreBlocksReset(void);
static void N3DS_HwVoiceMapReset(void);

static void N3DS_NdspSinkInit(void)
{
	Result rc;
	char line[96];
	float mix[12];

	if (s_ndspForcedOff)
	{
		return;
	}

	if (s_ndspInitAttempted)
	{
		/* Same idempotency contract as the worker thread below: Init is
		 * re-entered on every XA play request via NativeAudio_SpuInit. */
		return;
	}
	s_ndspInitAttempted = 1;

	rc = ndspInit();
	if (R_FAILED(rc))
	{
		/*
		 * The classic homebrew cause is a missing DSP firmware dump
		 * (sdmc:/3ds/dspfirm.cdc, dumped once with the DSP1 homebrew) --
		 * newer Luma3DS setups load it from NAND without the file, so on
		 * this project's target setup success is expected. Failing here is
		 * clean: the mixer keeps rendering (lifecycle stays exact) and
		 * every submit is refused, i.e. exactly the old muted behavior.
		 */
		snprintf(line, sizeof(line), "[CTR-3DS] ndsp init FAILED (0x%08lx) -- dspfirm.cdc? audio stays muted", (unsigned long)rc);
		N3DS_DebugLog(line);
		return;
	}

	s_ndspPcm = (s16 *)linearAlloc((size_t)N3DS_NDSP_BLOCK_COUNT * N3DS_NDSP_AGG_FRAMES * 2 * sizeof(s16));
	if (s_ndspPcm == NULL)
	{
		N3DS_DebugLog("[CTR-3DS] ndsp: linearAlloc failed, audio stays muted");
		ndspExit();
		return;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(N3DS_NDSP_CHANNEL);
	ndspChnSetInterp(N3DS_NDSP_CHANNEL, NDSP_INTERP_POLYPHASE);
	/* Starting rate only. The first submit re-pokes this to the XA track's
	 * own 37800/18900 (N3DS_NdspApplyChannelState). */
	ndspChnSetRate(N3DS_NDSP_CHANNEL, (float)N3DS_NDSP_MIXER_RATE_HZ);
	ndspChnSetFormat(N3DS_NDSP_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	memset(mix, 0, sizeof(mix));
	/* Memo (s_ndspGainLeft/Right) stays in RAW producer units; the output
	 * gain multiplies in only at the poke, here and in ApplyChannelState. */
	mix[0] = s_ndspOutputGain; /* front left */
	mix[1] = s_ndspOutputGain; /* front right */
	ndspChnSetMix(N3DS_NDSP_CHANNEL, mix);
	s_ndspRate = N3DS_NDSP_MIXER_RATE_HZ;
	s_ndspGainLeft = 1.0f;
	s_ndspGainRight = 1.0f;

	N3DS_HwVoiceMapReset();

	s_ndspUp = 1;
	snprintf(line, sizeof(line), "[CTR-3DS] ndsp: up (%d aggs x %d parcels x <=%d frames)", N3DS_NDSP_BLOCK_COUNT, N3DS_NDSP_AGG_PARCELS,
	         N3DS_NDSP_PARCEL_FRAMES_MAX);
	N3DS_DebugLog(line);
}

/*
 * Poke the music channel only when something actually changed -- the same
 * on-change discipline the hardware-voice mirror uses for its per-vblank
 * SetMix/SetRate updates.
 *
 * The rate change carries one deliberate side effect. XA pins its rate per
 * track, so a change means a new track has taken over the channel; anything
 * still queued is the previous track's tail, which the DSP would now replay
 * at the new pitch. Dropping it is more correct than pitch-shifting it, and
 * the partially-filled slot belongs to the old track as well.
 */
static int N3DS_NdspQueuedParcelsDspSide(void);

static void N3DS_NdspApplyChannelState(int sampleRate, float gainLeft, float gainRight)
{
	if (sampleRate != s_ndspRate)
	{
		char line[128];
		/* Named, not silent: this discard is a deliberate bounded loss, and an
		 * unexplained gap at a track change is exactly the kind of thing a log
		 * should account for rather than leave to inference. */
		int discarded = N3DS_NdspQueuedParcelsDspSide() + s_ndspAggParcels;

		ndspChnWaveBufClear(N3DS_NDSP_CHANNEL);
		/* Safe to zero: the clear above dropped the DSP's references to these
		 * buffers. Same reset N3DS_NdspSinkShutdown performs. */
		memset(s_ndspBufs, 0, sizeof(s_ndspBufs));
		memset(s_ndspBufParcels, 0, sizeof(s_ndspBufParcels));
		s_ndspNextBuf = 0;
		s_ndspAggParcels = 0;
		s_ndspAggFrames = 0;

		ndspChnSetRate(N3DS_NDSP_CHANNEL, (float)sampleRate);
		s_ndspRate = sampleRate;

		snprintf(line, sizeof(line), "[CTR-3DS] ndsp: music rate -> %d Hz (DSP resamples in hardware), dropped %d stale parcel(s)", sampleRate, discarded);
		N3DS_DebugLog(line);
	}

	if ((gainLeft != s_ndspGainLeft) || (gainRight != s_ndspGainRight))
	{
		float mix[12];

		memset(mix, 0, sizeof(mix));
		mix[0] = gainLeft * s_ndspOutputGain;
		mix[1] = gainRight * s_ndspOutputGain;
		ndspChnSetMix(N3DS_NDSP_CHANNEL, mix);
		s_ndspGainLeft = gainLeft;
		s_ndspGainRight = gainRight;
	}
}

static void N3DS_NdspSinkShutdown(void)
{
	/* Caller (N3DS_AudioThreadShutdown) has already joined the worker, and
	 * the fallback submit path runs on the same thread this does -- no
	 * submit can be in flight here. */
	if (s_ndspUp)
	{
		int v;

		for (v = 0; v < N3DS_HW_CHANNEL_COUNT; v++)
		{
			ndspChnWaveBufClear(v);
		}
		ndspChnWaveBufClear(N3DS_NDSP_CHANNEL);
		ndspExit();
	}
	if (s_ndspPcm != NULL)
	{
		linearFree(s_ndspPcm);
		s_ndspPcm = NULL;
	}
	if (s_hwStore != NULL)
	{
		linearFree(s_hwStore);
		s_hwStore = NULL;
		N3DS_HwStoreBlocksReset();
	}
	N3DS_HwVoiceMapReset();
	s_ndspUp = 0;
	s_ndspInitAttempted = 0; /* a later re-Init may bring the sink back up */
	s_ndspNextBuf = 0;
	s_ndspAggParcels = 0;
	s_ndspAggFrames = 0;
	s_ndspRate = 0;
	s_ndspGainLeft = -1.0f;
	s_ndspGainRight = -1.0f;
	memset(s_ndspBufs, 0, sizeof(s_ndspBufs));
	memset(s_ndspBufParcels, 0, sizeof(s_ndspBufParcels));
}

static int N3DS_NdspQueuedParcelsDspSide(void);

/* Hand the accumulated aggregate to the DSP and advance to the next pool
 * slot. Caller guarantees at least one parcel is accumulated and the current
 * slot is free (both checked by the submit path below). */
static void N3DS_NdspFlushAggregate(void)
{
	ndspWaveBuf *buf = &s_ndspBufs[s_ndspNextBuf];
	s16 *slot = s_ndspPcm + (size_t)s_ndspNextBuf * N3DS_NDSP_AGG_FRAMES * 2;
	u32 frames = (u32)s_ndspAggFrames;

	DSP_FlushDataCache(slot, frames * 2 * sizeof(s16)); /* once per aggregate, not per parcel */

	memset(buf, 0, sizeof(*buf));
	buf->data_pcm16 = slot;
	buf->nsamples = frames; /* per-channel frame count, not halfword total */
	buf->looping = false;
	ndspChnWaveBufAdd(N3DS_NDSP_CHANNEL, buf);

	/* Remembered per buffer rather than recomputed from nsamples: parcels are
	 * no longer a fixed frame count, so nsamples/PARCEL_FRAMES would be wrong
	 * (and it was a division on the queue-depth query's hot path). */
	s_ndspBufParcels[s_ndspNextBuf] = s_ndspAggParcels;
	s_ndspNextBuf = (s_ndspNextBuf + 1) % N3DS_NDSP_BLOCK_COUNT;
	s_ndspAggParcels = 0;
	s_ndspAggFrames = 0;
}

/* Flush a partially-filled aggregate, if any. Called by the worker when XA
 * goes idle -- without this, the last 1..7 parcels of a track (the tail of
 * an announcer line) would sit unheard in the accumulating slot until the
 * NEXT track's parcels pushed them out. No-op when empty or the slot is
 * still held by the DSP (then the parcels are simply not lost -- they flush
 * with the next append). */
static void N3DS_NdspFlushPending(void)
{
	if (!s_ndspUp || (s_ndspAggParcels <= 0))
	{
		return;
	}
	if ((s_ndspBufs[s_ndspNextBuf].status == NDSP_WBUF_QUEUED) || (s_ndspBufs[s_ndspNextBuf].status == NDSP_WBUF_PLAYING))
	{
		return;
	}
	N3DS_NdspFlushAggregate();
}

int N3DS_NdspSubmitMusicParcel(const short *frames, int frameCount, int sampleRate, float gainLeft, float gainRight)
{
	ndspWaveBuf *buf;
	s16 *slot;

	if (!s_ndspUp)
	{
		return 0;
	}
	if ((frames == NULL) || (frameCount <= 0) || (frameCount > N3DS_NDSP_PARCEL_FRAMES_MAX) || (sampleRate <= 0))
	{
		/* Producer disagrees with the hand-matched bound (see
		 * N3DS_NDSP_PARCEL_FRAMES_MAX) -- refuse loudly-in-counters rather
		 * than under/overrun the slot. */
		s_ndspDroppedBlocks++;
		return 0;
	}

	/* Before reading s_ndspNextBuf: a rate change resets the pool. */
	N3DS_NdspApplyChannelState(sampleRate, gainLeft, gainRight);

	buf = &s_ndspBufs[s_ndspNextBuf];
	if ((buf->status == NDSP_WBUF_QUEUED) || (buf->status == NDSP_WBUF_PLAYING))
	{
		/* Pool full: drop the newest parcel (this one), never stall the
		 * mixer -- see the N3DS_NDSP_BLOCK_COUNT comment. */
		s_ndspDroppedBlocks++;
		return 0;
	}

	/* Slot cannot hold this parcel as well: hand off what is accumulated and
	 * start a fresh one, so an aggregate is always contiguous. Unreachable at
	 * a steady parcel size (AGG_PARCELS x PARCEL_FRAMES_MAX is exactly one
	 * slot); reachable if the parcel size grew mid-slot. */
	if ((s_ndspAggFrames + frameCount) > N3DS_NDSP_AGG_FRAMES)
	{
		s_ndspFullFlushes++;
		N3DS_NdspFlushAggregate();
		buf = &s_ndspBufs[s_ndspNextBuf];
		if ((buf->status == NDSP_WBUF_QUEUED) || (buf->status == NDSP_WBUF_PLAYING))
		{
			s_ndspDroppedBlocks++;
			return 0;
		}
	}

	slot = s_ndspPcm + ((size_t)s_ndspNextBuf * N3DS_NDSP_AGG_FRAMES + (size_t)s_ndspAggFrames) * 2;
	memcpy(slot, frames, (size_t)frameCount * 2 * sizeof(s16));
	s_ndspAggFrames += frameCount;
	s_ndspAggParcels++;

	/*
	 * Flush when full -- or EARLY when the DSP is about to run dry, so a
	 * track start (or recovery after a stall) begins playing on the next
	 * parcel instead of waiting ~133ms for a full aggregate. The early
	 * flush produces one small buffer; the queue behind it refills with
	 * full-size ones, so the seam rate settles back to ~7.5/sec.
	 */
	if (s_ndspAggParcels >= N3DS_NDSP_AGG_DEFAULT)
	{
		s_ndspFullFlushes++;
		N3DS_NdspFlushAggregate();
	}
	else if (N3DS_NdspQueuedParcelsDspSide() == 0)
	{
		/* The DSP already ran dry -- a real playback gap. Count it (the pop
		 * signal) and flush the partial to recover as fast as possible. */
		s_ndspDspEmptyEvents++;
		s_ndspEarlyFlushes++;
		N3DS_NdspFlushAggregate();
	}

	return 1;
}

int N3DS_NdspIsUp(void)
{
	return s_ndspUp;
}

/* DSP-side queued parcels only (handed-over wave buffers, counted in vblanks
 * of audio) -- the early-flush trigger: 0 means the channel is running dry. */
static int N3DS_NdspQueuedParcelsDspSide(void)
{
	int queued = 0;
	int i;

	for (i = 0; i < N3DS_NDSP_BLOCK_COUNT; i++)
	{
		if ((s_ndspBufs[i].status == NDSP_WBUF_QUEUED) || (s_ndspBufs[i].status == NDSP_WBUF_PLAYING))
		{
			queued += s_ndspBufParcels[i];
		}
	}

	return queued;
}

int N3DS_NdspGetQueuedBlocks(void)
{
	/* Still reported in PARCEL units (one vblank of audio each) even though
	 * the pool holds aggregates -- the worker's ~12-parcel cushion target
	 * and the ndspQ= log line both predate aggregation and keep their
	 * meaning, at any source rate. Unflushed parcels in the accumulating
	 * slot count too: they are already in the sink, just not handed to the
	 * DSP yet. */
	return N3DS_NdspQueuedParcelsDspSide() + s_ndspAggParcels;
}

int N3DS_NdspGetDroppedBlocks(void)
{
	return (int)s_ndspDroppedBlocks;
}

int N3DS_AudioSetOutputGainPercent(int percent)
{
	if (percent >= 0)
	{
		if (percent > 400)
		{
			percent = 400;
		}
		s_ndspOutputGain = (float)percent / 100.0f;
	}
	else
	{
		s_ndspOutputGain = N3DS_NDSP_OUTPUT_GAIN_DEFAULT;
	}

	/*
	 * Force the music channel's on-change memo stale so the next submit
	 * re-pokes SetMix with the new gain. Hardware voices pick it up as the
	 * per-vblank mirror next changes each voice's volume -- in-game volumes
	 * shift constantly, so that lands within moments. Cross-thread against
	 * the submit thread's own memo writes, but both sides only ever converge
	 * on a re-poke; worst case is one redundant SetMix.
	 */
	s_ndspGainLeft = -1.0f;
	s_ndspGainRight = -1.0f;

	return (int)(s_ndspOutputGain * 100.0f + 0.5f);
}

void N3DS_NdspGetFlushCounts(int *fullFlushes, int *earlyFlushes)
{
	if (fullFlushes != NULL)
	{
		*fullFlushes = (int)s_ndspFullFlushes;
	}
	if (earlyFlushes != NULL)
	{
		*earlyFlushes = (int)s_ndspEarlyFlushes;
	}
}

int N3DS_NdspGetDspEmptyEvents(void)
{
	return (int)s_ndspDspEmptyEvents;
}

/*
 * ---- Hardware voices (plan B) --------------------------------------------
 *
 * The PS1's SPU was an autonomous mixer chip; software-mixing its 24 voices
 * at 44.1kHz measured ~4x over real-time on this CPU even on a dedicated
 * core (see PORT_FEASIBILITY_3DS.md). So the DSP's own 24
 * channels do the playing, and the CPU is reduced to what it was on retail
 * hardware: poking pitch/volume registers and starting/stopping notes.
 * native_audio.c's per-vblank mirror decides WHAT to poke (it owns voice
 * state); this side owns everything libctru: the linear-memory sample
 * store the DSP reads from, wave-buffer chains, and the channel calls.
 *
 * Threading: every entry point here runs under native_audio.c's output
 * lock -- the mixing thread through the per-vblank mirror, the game thread
 * through SpuSetKey/SpuWrite -- so the store's block table and the
 * voice<->channel map need no lock of their own. ndspChn* calls are
 * internally locked by libctru.
 */

/*
 * Decoded PCM16 the DSP can address must live in linear memory, and linear
 * headroom after the renderer's atlases is ~4MB, so growing the store is not
 * a lever -- reclaiming is. The entire PS1 SPU RAM is 512KB of ADPCM ~= 1.75MB
 * decoded, so 3MB (declared above with the sink statics -- shutdown frees it)
 * holds a full bank of decodes with slack provided abandoned slots come back.
 * Blocks are handed out best-fit from the table and coalesced with their free
 * neighbours on release; the tail cursor advances only when nothing free fits.
 * The whole store also resets when the game re-uploads over cached regions
 * (bank reload), which silences every hardware channel first so the DSP never
 * reads freed samples.
 */
static u32 s_hwStoreResets;   /* diagnostic */
static u32 s_hwStoreFullDrops;
static u32 s_hwVoiceDenied;   /* key-ons dropped because every channel was still sounding */
static u32 s_hwVoiceRebinds;  /* key-ons that took an idle channel away from another voice */

/* Two wave buffers per channel: [start..loopEnd) once, then [loopStart..
 * loopEnd) looping forever until the CPU says stop. One-shots use only
 * the first. Indexed by CHANNEL: the DSP holds these pointers for as long
 * as it owns the channel, which outlives any one voice's claim on it. */
static ndspWaveBuf s_hwBufs[N3DS_HW_CHANNEL_COUNT][2];

/* Voice <-> channel map. -1 means unbound, which is not the zero-init every
 * other static here gets, so N3DS_HwVoiceMapReset seeds both directions from
 * sink init, store reset and shutdown. */
static int s_hwVoiceChan[N3DS_SPU_VOICE_COUNT];
static int s_hwChanVoice[N3DS_HW_CHANNEL_COUNT];
/* Key-on ordinal per channel; the victim search takes the smallest, i.e. the
 * idle channel that has gone unused longest. */
static u32 s_hwChanSeq[N3DS_HW_CHANNEL_COUNT];
static u32 s_hwChanSeqNext;

static void N3DS_HwVoiceMapReset(void)
{
	int i;

	for (i = 0; i < N3DS_SPU_VOICE_COUNT; i++)
	{
		s_hwVoiceChan[i] = -1;
	}
	for (i = 0; i < N3DS_HW_CHANNEL_COUNT; i++)
	{
		s_hwChanVoice[i] = -1;
		s_hwChanSeq[i] = 0;
	}
	s_hwChanSeqNext = 0;
}

static void N3DS_HwStoreBlocksReset(void)
{
	memset(s_hwStoreBlocks, 0, sizeof(s_hwStoreBlocks));
	s_hwStoreBlockCount = 0;
	s_hwStoreHiWater = 0;
	s_hwStoreLive = 0;
}

short *N3DS_HwVoiceStoreAlloc(int sampleCount)
{
	u32 bytes;
	int best = -1;
	int i;

	if (s_hwStore == NULL)
	{
		s_hwStore = (s16 *)linearAlloc(N3DS_HW_STORE_BYTES);
		if (s_hwStore == NULL)
		{
			N3DS_DebugLog("[CTR-3DS] hw-voice store: linearAlloc FAILED -- voices silent");
			return NULL;
		}
		N3DS_HwStoreBlocksReset();
	}
	if (sampleCount <= 0)
	{
		return NULL;
	}

	bytes = ((u32)sampleCount * sizeof(s16) + 31u) & ~31u; /* cache-line align keeps flushes exact */

	for (i = 0; i < s_hwStoreBlockCount; i++)
	{
		if (s_hwStoreBlocks[i].inUse || (s_hwStoreBlocks[i].bytes < bytes))
		{
			continue;
		}
		if ((best < 0) || (s_hwStoreBlocks[i].bytes < s_hwStoreBlocks[best].bytes))
		{
			best = i;
		}
	}

	if (best >= 0)
	{
		u32 remainder = s_hwStoreBlocks[best].bytes - bytes;

		/* Split only when the tail is big enough to serve a later sample and
		 * a record is free to track it; otherwise the caller keeps the slack
		 * rather than stranding an untracked hole. */
		if ((remainder >= 64u) && (s_hwStoreBlockCount < N3DS_HW_STORE_BLOCKS))
		{
			memmove(&s_hwStoreBlocks[best + 2], &s_hwStoreBlocks[best + 1], (size_t)(s_hwStoreBlockCount - best - 1) * sizeof(s_hwStoreBlocks[0]));
			s_hwStoreBlocks[best + 1].off = s_hwStoreBlocks[best].off + bytes;
			s_hwStoreBlocks[best + 1].bytes = remainder;
			s_hwStoreBlocks[best + 1].inUse = 0;
			s_hwStoreBlocks[best].bytes = bytes;
			s_hwStoreBlockCount++;
		}
		s_hwStoreBlocks[best].inUse = 1;
		s_hwStoreLive += s_hwStoreBlocks[best].bytes;
		return (short *)((u8 *)s_hwStore + s_hwStoreBlocks[best].off);
	}

	if (((s_hwStoreHiWater + bytes) > N3DS_HW_STORE_BYTES) || (s_hwStoreBlockCount >= N3DS_HW_STORE_BLOCKS))
	{
		s_hwStoreFullDrops++;
		return NULL;
	}

	s_hwStoreBlocks[s_hwStoreBlockCount].off = s_hwStoreHiWater;
	s_hwStoreBlocks[s_hwStoreBlockCount].bytes = bytes;
	s_hwStoreBlocks[s_hwStoreBlockCount].inUse = 1;
	s_hwStoreBlockCount++;
	s_hwStoreHiWater += bytes;
	s_hwStoreLive += bytes;

	return (short *)((u8 *)s_hwStore + s_hwStoreBlocks[s_hwStoreBlockCount - 1].off);
}

void N3DS_HwVoiceStoreFree(const short *pcm)
{
	u32 off;
	int i;

	if ((s_hwStore == NULL) || (pcm == NULL))
	{
		return;
	}

	off = (u32)((const u8 *)pcm - (const u8 *)s_hwStore);
	for (i = 0; i < s_hwStoreBlockCount; i++)
	{
		if (s_hwStoreBlocks[i].inUse && (s_hwStoreBlocks[i].off == off))
		{
			break;
		}
	}
	if (i >= s_hwStoreBlockCount)
	{
		/* A pointer from before a store reset: its block is already gone. */
		return;
	}

	s_hwStoreBlocks[i].inUse = 0;
	s_hwStoreLive -= s_hwStoreBlocks[i].bytes;

	/* Coalesce forward first, then backward, so a run of releases leaves one
	 * block rather than a fragment per sample. */
	if (((i + 1) < s_hwStoreBlockCount) && !s_hwStoreBlocks[i + 1].inUse)
	{
		s_hwStoreBlocks[i].bytes += s_hwStoreBlocks[i + 1].bytes;
		memmove(&s_hwStoreBlocks[i + 1], &s_hwStoreBlocks[i + 2], (size_t)(s_hwStoreBlockCount - i - 2) * sizeof(s_hwStoreBlocks[0]));
		s_hwStoreBlockCount--;
	}
	if ((i > 0) && !s_hwStoreBlocks[i - 1].inUse)
	{
		s_hwStoreBlocks[i - 1].bytes += s_hwStoreBlocks[i].bytes;
		memmove(&s_hwStoreBlocks[i], &s_hwStoreBlocks[i + 1], (size_t)(s_hwStoreBlockCount - i - 1) * sizeof(s_hwStoreBlocks[0]));
		s_hwStoreBlockCount--;
	}
}

void N3DS_HwVoiceStoreCommit(const short *pcm, int sampleCount)
{
	DSP_FlushDataCache(pcm, (u32)sampleCount * sizeof(s16));
}

void N3DS_HwVoiceStoreReset(void)
{
	int ch;

	/* Channels first: the store's contents are about to be recycled. Skipped
	 * with the sink down (dumpreq `ndspoff`) -- the DSP owns nothing to
	 * silence and ndspChnWaveBufClear would touch a torn-down channel. */
	if (s_ndspUp)
	{
		for (ch = 0; ch < N3DS_HW_CHANNEL_COUNT; ch++)
		{
			ndspChnWaveBufClear(ch);
		}
	}
	memset(s_hwBufs, 0, sizeof(s_hwBufs));
	N3DS_HwVoiceMapReset();
	N3DS_HwStoreBlocksReset();
	s_hwStoreResets++;
}

/*
 * A channel is reusable once nothing it was handed is still due to play:
 * no voice bound, or every wave buffer consumed. libctru's own ndsp thread
 * writes those statuses from the DSP's frame, so this read is deliberately
 * unsynchronised -- being one audio frame stale can only delay a rebind,
 * never hand out a channel that is still sounding. A looping buffer never
 * leaves PLAYING, so a looping voice holds its channel until the CPU stops it.
 */
static int N3DS_HwChannelIsIdle(int ch)
{
	int i;

	if (s_hwChanVoice[ch] < 0)
	{
		return 1;
	}
	for (i = 0; i < 2; i++)
	{
		if ((s_hwBufs[ch][i].status == NDSP_WBUF_QUEUED) || (s_hwBufs[ch][i].status == NDSP_WBUF_PLAYING))
		{
			return 0;
		}
	}

	return 1;
}

/*
 * Channel for a key-on: the one this voice already holds (retrigger), else
 * the idle channel unused longest. -1 means all 23 are still sounding, the
 * only case that costs a sound.
 */
static int N3DS_HwChannelAcquire(int voice)
{
	int best = -1;
	int ch;

	if (s_hwVoiceChan[voice] >= 0)
	{
		s_hwChanSeq[s_hwVoiceChan[voice]] = ++s_hwChanSeqNext;
		return s_hwVoiceChan[voice];
	}

	for (ch = 0; ch < N3DS_HW_CHANNEL_COUNT; ch++)
	{
		if (!N3DS_HwChannelIsIdle(ch))
		{
			continue;
		}
		if ((best < 0) || (s_hwChanSeq[ch] < s_hwChanSeq[best]))
		{
			best = ch;
		}
	}
	if (best < 0)
	{
		return -1;
	}

	if (s_hwChanVoice[best] >= 0)
	{
		/* The previous owner keeps its lifecycle and loses only its output;
		 * its later SetRate/SetMix/Stop become no-ops through the map. */
		s_hwVoiceChan[s_hwChanVoice[best]] = -1;
		s_hwVoiceRebinds++;
	}
	s_hwChanVoice[best] = voice;
	s_hwVoiceChan[voice] = best;
	s_hwChanSeq[best] = ++s_hwChanSeqNext;

	return best;
}

static int N3DS_HwVoiceChannel(int voice)
{
	if (!s_ndspUp || (voice < 0) || (voice >= N3DS_SPU_VOICE_COUNT))
	{
		return -1;
	}

	return s_hwVoiceChan[voice];
}

void N3DS_HwVoiceStart(int voice, const short *pcm, int sampleCount, int loopStart, int loopEnd, int loopEnabled, int startSample, float rateHz)
{
	ndspWaveBuf *bufs;
	int firstEnd;
	int ch;

	if (!s_ndspUp || (voice < 0) || (voice >= N3DS_SPU_VOICE_COUNT) || (pcm == NULL) || (sampleCount <= 0))
	{
		return;
	}

	if (startSample < 0)
	{
		startSample = 0;
	}
	if (startSample >= sampleCount)
	{
		return;
	}

	ch = N3DS_HwChannelAcquire(voice);
	if (ch < 0)
	{
		s_hwVoiceDenied++;
		return;
	}

	bufs = s_hwBufs[ch];
	ndspChnWaveBufClear(ch); /* retrigger = PS1 keyon semantics */
	/* Both slots, not just the one about to be filled: the previous sound on
	 * this channel may have queued the loop buffer. */
	memset(bufs, 0, sizeof(s_hwBufs[0]));
	ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE);
	ndspChnSetRate(ch, rateHz);

	/* First segment: from the start offset up to the loop end (or the
	 * whole sample when not looping). The PS1 plays [start..loopEnd) then
	 * jumps to loopStart; a start offset past loopEnd (restored state)
	 * just plays out the tail before looping normally. */
	firstEnd = (loopEnabled && (loopEnd > startSample) && (loopEnd <= sampleCount)) ? loopEnd : sampleCount;

	bufs[0].data_pcm16 = (s16 *)pcm + startSample;
	bufs[0].nsamples = (u32)(firstEnd - startSample);
	bufs[0].looping = false;
	ndspChnWaveBufAdd(ch, &bufs[0]);

	if (loopEnabled && (loopEnd > loopStart) && (loopEnd <= sampleCount) && (loopStart >= 0))
	{
		bufs[1].data_pcm16 = (s16 *)pcm + loopStart;
		bufs[1].nsamples = (u32)(loopEnd - loopStart);
		bufs[1].looping = true;
		ndspChnWaveBufAdd(ch, &bufs[1]);
	}
}

void N3DS_HwVoiceSetRate(int voice, float rateHz)
{
	int ch = N3DS_HwVoiceChannel(voice);

	if (ch < 0)
	{
		return;
	}
	ndspChnSetRate(ch, rateHz);
}

void N3DS_HwVoiceSetMix(int voice, float left, float right)
{
	float mix[12];
	int ch = N3DS_HwVoiceChannel(voice);

	if (ch < 0)
	{
		return;
	}
	memset(mix, 0, sizeof(mix));
	mix[0] = left * s_ndspOutputGain;
	mix[1] = right * s_ndspOutputGain;
	ndspChnSetMix(ch, mix);
}

void N3DS_HwVoiceStop(int voice)
{
	int ch = N3DS_HwVoiceChannel(voice);

	if (ch < 0)
	{
		return;
	}
	ndspChnWaveBufClear(ch);
	memset(s_hwBufs[ch], 0, sizeof(s_hwBufs[0]));
	/* Unbind both ways: the channel is free for the next key-on immediately,
	 * and this voice's pokes stop reaching whoever takes it. */
	s_hwChanVoice[ch] = -1;
	s_hwVoiceChan[voice] = -1;
}

int N3DS_HwVoiceGetDenied(void)
{
	return (int)s_hwVoiceDenied;
}

int N3DS_HwVoiceGetRebinds(void)
{
	return (int)s_hwVoiceRebinds;
}

int N3DS_HwVoiceStoreGetUsedKiB(void)
{
	return (int)(s_hwStoreLive >> 10);
}

int N3DS_HwVoiceStoreGetHiWaterKiB(void)
{
	return (int)(s_hwStoreHiWater >> 10);
}

int N3DS_HwVoiceStoreGetDrops(void)
{
	return (int)s_hwStoreFullDrops;
}

int N3DS_HwVoiceStoreGetResets(void)
{
	return (int)s_hwStoreResets;
}

/*
 * ---- Per-wake budget profile ---------------------------------------------
 *
 * Hardware run 6 proved the worker is the frame governor on XA-music tracks
 * but could not say WHICH of its five jobs eats the wake: lifecycle drain,
 * the 23-channel mirror, XA parcel render, the SD pump, or simply waiting on
 * the render lock the game thread is holding (finding #2 of run 1). Each
 * gets a bucket. Written only by the worker; drained and zeroed by
 * N3DS_AudioThreadGetProfile for the renderer's periodic shape line, so the
 * numbers reported are per-window, not since-boot.
 *
 * Cost of the instrumentation itself: ~6 svcGetSystemTick calls per wake, so
 * ~360/sec -- three orders of magnitude below the per-triangle scope tax
 * the renderer had to gate off.
 */
static u64 s_profLockWaitTicks;
static u64 s_profRenderTicks;

/*
 * Take the render lock, charging the wait to the worker's profile. The
 * parcel render and the XA pump take this same lock through
 * N3DS_AudioLock rather than directly, so that function routes worker-thread
 * callers here too -- otherwise a worker blocked behind a main-thread bulk
 * load would show up as expensive XA work instead of as contention, which is
 * exactly the misreading run 1's finding #2 warns about. `parcelUs`/`pumpUs`
 * are therefore wall time INCLUDING any wait; subtract lockWaitUs to get
 * work.
 */
static void N3DS_AudioRenderLockTimed(void);
static u64 s_profParcelTicks;
static u64 s_profPumpTicks;
static u32 s_profWakes;
static u32 s_profVBlanks;
static u32 s_profParcels;

unsigned long long N3DS_AudioTicksNow(void)
{
	return (unsigned long long)svcGetSystemTick();
}

void N3DS_AudioThreadGetProfile(struct N3DS_AudioWorkerProfile *out)
{
	unsigned long long lifecycleTicks = 0;
	unsigned long long mirrorTicks = 0;

	if (out == NULL)
	{
		return;
	}

	NativeAudio_TakeWorkerProfileForAudioThread(&lifecycleTicks, &mirrorTicks);

	out->wakes = (int)s_profWakes;
	out->vblanks = (int)s_profVBlanks;
	out->parcels = (int)s_profParcels;
	out->lockWaitUs = (int)(s_profLockWaitTicks / (SYSCLOCK_ARM11 / 1000000ULL));
	out->renderUs = (int)(s_profRenderTicks / (SYSCLOCK_ARM11 / 1000000ULL));
	out->lifecycleUs = (int)(lifecycleTicks / (SYSCLOCK_ARM11 / 1000000ULL));
	out->mirrorUs = (int)(mirrorTicks / (SYSCLOCK_ARM11 / 1000000ULL));
	out->parcelUs = (int)(s_profParcelTicks / (SYSCLOCK_ARM11 / 1000000ULL));
	out->pumpUs = (int)(s_profPumpTicks / (SYSCLOCK_ARM11 / 1000000ULL));

	s_profLockWaitTicks = 0;
	s_profRenderTicks = 0;
	s_profParcelTicks = 0;
	s_profPumpTicks = 0;
	s_profWakes = 0;
	s_profVBlanks = 0;
	s_profParcels = 0;
}

/*
 * Worker cushion target in parcels, clamped to what the pool can hold WITHOUT
 * dropping at the current aggregate size: (BLOCK_COUNT-1) full buffers, leaving
 * one slot free for the accumulating flush. Without the clamp a small target
 * makes pool capacity fall below the 12-parcel (~200ms) target, so the worker's
 * top-up loop can never reach it, spins to its guard cap, and every spin
 * advances the game-visible XA clock (NativeAudio_RenderXAParcelForAudioThread)
 * for a parcel the full sink then silently drops -- racing the music several
 * times past real speed while the game clock is unaffected. At the shipping
 * target this evaluates to 12. The deeper bug -- the XA
 * clock advancing even when a submit is dropped -- still lurks for any
 * pool-full stall; this only keeps the pool from filling in normal operation.
 */
static int N3DS_NdspCushionTarget(void)
{
	int cushion = (N3DS_NDSP_BLOCK_COUNT - 1) * N3DS_NDSP_AGG_DEFAULT;

	return (cushion < 12) ? cushion : 12;
}

/*
 * Cross-thread stall watchdog (Rosalina-freeze hardening). This
 * thread's own wait (svcWaitSynchronization
 * above) is capped at one XA period, so it can never wedge itself -- which is
 * what makes it a trustworthy place to notice ANOTHER thread that has: if
 * main or the render worker's heartbeat counter has not moved since the last
 * check while this thread kept waking normally, one of them is stuck in an
 * untimed wait (see N3DS_GuardedFrameBegin, n3ds_backend.c). A thread that
 * has not yet made its first mark (main before its first frame, a worker
 * that never started) reads as a counter at 0, which must never be reported
 * as a stall.
 *
 * wakeCount is the caller's own dedicated counter, not s_profWakes -- that one
 * is drained to 0 by N3DS_AudioThreadGetProfile on an unrelated cadence and
 * would desync this function's every-64 gate and its logged `aw=` value.
 */
static void N3DS_BeatWatchdogCheck(unsigned int wakeCount)
{
	static unsigned int s_awPrevMain;
	static unsigned int s_awPrevRw;
	static u64 s_awStallStartTick;
	static u64 s_awLastLogTick;
	unsigned int mainCounter = 0;
	int mainWhere = N3DS_BEAT_WHERE_OK;
	unsigned int rwCounter = 0;
	int rwWhere = N3DS_BEAT_WHERE_OK;
	int mainStuck;
	int rwStuck;

	N3DS_BeatRead(N3DS_BEAT_THREAD_MAIN, &mainCounter, &mainWhere);
	N3DS_BeatRead(N3DS_BEAT_THREAD_RENDERWORKER, &rwCounter, &rwWhere);

	mainStuck = (mainCounter != 0) && (mainCounter == s_awPrevMain);
	rwStuck = (rwCounter != 0) && (rwCounter == s_awPrevRw);

	if (mainStuck || rwStuck)
	{
		const u64 now = svcGetSystemTick();

		if (s_awStallStartTick == 0)
		{
			s_awStallStartTick = now;
			s_awLastLogTick = 0;
		}

		if ((((now - s_awLastLogTick) * 1000ull) / SYSCLOCK_ARM11) >= 2000ull)
		{
			const unsigned int ageMs = (unsigned int)(((now - s_awStallStartTick) * 1000ull) / SYSCLOCK_ARM11);
			char msg[128];

			snprintf(msg, sizeof(msg), "[BEAT] STALL main=%u@%s rw=%u@%s aw=%u ageMs=%u", mainCounter, N3DS_BeatWhereName(mainWhere),
			         rwCounter, N3DS_BeatWhereName(rwWhere), wakeCount, ageMs);
			N3DS_DebugLog(msg);
			N3DS_LogFlushNow();
			s_awLastLogTick = now;
		}
	}
	else
	{
		s_awStallStartTick = 0;
	}

	s_awPrevMain = mainCounter;
	s_awPrevRw = rwCounter;
}

/*
 * Cushion-driven XA: top the music channel's queue up to the cushion target
 * whenever XA has content. A stall on the calling thread (an SD read in the
 * pump, a slow frame when there is no worker) drains cushion instead of
 * gapping the audio, and the next call refills it; nothing is discarded. XA
 * position therefore runs up to the cushion ahead of real time, as the PS1's
 * CD buffer did. The guard bounds one call so a long stall is followed by a
 * refill over a few calls, not one burst. Takes the render lock itself per
 * parcel: call with NO locks held.
 */
static void N3DS_AudioXaTopUp(void)
{
	u64 mark = svcGetSystemTick();
	int guard = 0;
	int xaIdle = 0;

	while ((N3DS_NdspGetQueuedBlocks() < N3DS_NdspCushionTarget()) && (guard < 8))
	{
		/* 0: XA idle, or the track just ended. -1 (no worker only): the next
		 * parcel is not decoded yet. Either way the DSP gets what is pending
		 * rather than leaving it in the aggregate slot. */
		if (NativeAudio_RenderXAParcelForAudioThread() <= 0)
		{
			xaIdle = 1;
			break;
		}
		guard++;
	}
	if (xaIdle)
	{
		N3DS_NdspFlushPending(); /* play out a track's tail parcels */
	}
	s_profParcelTicks += svcGetSystemTick() - mark;
	s_profParcels += (u32)guard;
}

void N3DS_AudioInlineStepVBlank(void)
{
	u64 mark;

	if (s_audioThreadRunning)
	{
		return; /* the worker owns every step below */
	}
	s_profWakes++;

	N3DS_AudioRenderLockTimed();
	mark = svcGetSystemTick();
	NativeAudio_RenderVBlankForAudioThread();
	LightLock_Unlock(&s_audioRenderLock);
	s_profRenderTicks += svcGetSystemTick() - mark;
	s_profVBlanks++;

	/* Each of the three below takes the render lock itself. */
	N3DS_AudioXaTopUp();

	mark = svcGetSystemTick();
	NativeAudio_XAPumpForAudioThread();
	s_profPumpTicks += svcGetSystemTick() - mark;

	NativeAudio_HwDecodePumpForAudioThread();
}

static void N3DS_AudioThreadMain(void *arg)
{
	const u64 xaPeriodTicks = SYSCLOCK_ARM11 / 60;
	u64 nextXaTick = svcGetSystemTick() + xaPeriodTicks;

	(void)arg;

	for (;;)
	{
		u64 now = svcGetSystemTick();
		s64 waitNs = 0;

		if ((s64)(nextXaTick - now) > 0)
		{
			waitNs = (s64)((nextXaTick - now) * 1000000000ULL / SYSCLOCK_ARM11);
		}
		svcWaitSynchronization(s_audioWakeHandle, waitNs); /* wake on vblank signal OR the XA deadline */
		svcClearEvent(s_audioWakeHandle);
		if (s_audioThreadShouldStop)
		{
			break;
		}
		s_profWakes++;

		{
			/* Independent of s_profWakes -- see N3DS_BeatWatchdogCheck's own
			 * comment for why that one can't double as this gate. */
			static unsigned int s_beatWakeCounter;

			N3DS_BeatMark(N3DS_BEAT_THREAD_AUDIO, N3DS_BEAT_WHERE_AUDIO_RENDER);
			s_beatWakeCounter++;
			if ((s_beatWakeCounter % 64) == 0)
			{
				N3DS_BeatWatchdogCheck(s_beatWakeCounter);
			}
		}

		for (;;) /* drain the current lifecycle backlog, one vblank at a time */
		{
			int hasWork;

			LightLock_Lock(&s_audioCounterLock);
			hasWork = (s_renderedVBlanks < s_targetVBlanks);
			LightLock_Unlock(&s_audioCounterLock);
			if (!hasWork)
			{
				break;
			}

			N3DS_AudioRenderLockTimed(); /* backs N3DS_AudioLock / NativeAudio_LockOutput */
			now = svcGetSystemTick();
			NativeAudio_RenderVBlankForAudioThread();
			LightLock_Unlock(&s_audioRenderLock); /* released BEFORE the next iteration -- this is
			                                          the "what changed from v1" fix: the render
			                                          lock is never held across the whole drain. */
			s_profRenderTicks += svcGetSystemTick() - now;
			s_profVBlanks++;

			LightLock_Lock(&s_audioCounterLock);
			s_renderedVBlanks++;
			LightLock_Unlock(&s_audioCounterLock);
		}

		N3DS_AudioXaTopUp();
		now = svcGetSystemTick();
		nextXaTick = now + xaPeriodTicks; /* next top-up check one vblank out */

		NativeAudio_XAPumpForAudioThread(); /* once per wake, burst-refilling inside; run with NO
		                                        locks held -- NativeAudio_XAStreamPump takes the
		                                        render lock itself via N3DS_AudioLock, and LightLock
		                                        is non-recursive. */
		s_profPumpTicks += svcGetSystemTick() - now;

		NativeAudio_HwDecodePumpForAudioThread(); /* once per wake, no locks held -- takes the
		                                             render lock itself in chunk-sized holds */
	}

	threadExit(0);
}

void N3DS_AudioThreadInit(void)
{
	bool isNew3DS;
	s32 mainPriority;
	s32 audioPriority;
	int audioCore;

	if (s_audioThreadRunning)
	{
		/*
		 * Idempotent by design, not by accident: NativeAudio_SpuInit (this
		 * function's caller) runs on every NativeAudio_PlayXATrack/
		 * PlayXAFile call, not just once at boot -- i.e. routinely many
		 * times per play session. A worker thread is already alive; leave
		 * it running exactly as-is. Re-running LightLock_Init below on
		 * locks the worker may currently hold, or calling threadCreate
		 * again, would corrupt lock state and orphan/leak the existing
		 * thread (it would still be running, but s_audioThread would no
		 * longer point at it, so Shutdown could never join it).
		 */
		return;
	}

	/*
	 * Locks/event/counters are real and initialized unconditionally from
	 * here down, no matter what the checks below decide --
	 * N3DS_AudioLock/N3DS_AudioUnlock (backing SDL_LockAudioStream/
	 * UnlockAudioStream) are reachable from ~30 existing native_audio.c call
	 * sites regardless of whether a worker thread ends up existing, and
	 * locking an uninitialized LightLock is not a defined operation.
	 */
	LightLock_Init(&s_audioRenderLock);
	LightLock_Init(&s_audioCounterLock);
	s_targetVBlanks = 0;
	s_renderedVBlanks = 0;
	s_droppedVBlanks = 0;
	s_audioThreadShouldStop = 0;
	s_audioThread = NULL;
	s_audioThreadRunning = 0;

	/*
	 * The ndsp sink comes up regardless of whether a worker thread does:
	 * the game thread drives it (N3DS_AudioInlineStepVBlank) when
	 * no worker could be secured. Guarded internally against the repeat
	 * calls described above.
	 */
	N3DS_NdspSinkInit();

	/* The worker's wake event is a kernel handle (see its declaration);
	 * created once and kept across repeat Init calls. Without it there can
	 * be no worker, but the sink above stays up and the game thread drives
	 * it. */
	if (s_audioWakeHandle == 0)
	{
		if (R_FAILED(svcCreateEvent(&s_audioWakeHandle, RESET_STICKY)))
		{
			s_audioWakeHandle = 0;
			N3DS_DebugLog("[CTR-3DS] audio worker: NO (svcCreateEvent failed) -- game thread drives the DSP");
			return;
		}
	}

	isNew3DS = false;
	APT_CheckNew3DS(&isNew3DS);
	if (!isNew3DS)
	{
		/* This project targets New3DS-XL only, but fail safe rather than
		 * assume -- never fall back to a same-core thread (see "What
		 * changed from v1" in the design doc: core_id=-1 does NOT mean "any
		 * free core", it means "all CPUs", which resolves to core 0 here). */
		N3DS_DebugLog("[CTR-3DS] audio worker: NO (APT says not New3DS) -- game thread drives the DSP");
		return;
	}

	/*
	 * Cores 2 and 3 are the New3DS's app-available cores; core 1 is the
	 * system core, shared with the sysmodules and with Luma's Rosalina menu
	 * thread. Reserving core 1 for the app (APT_SetAppCpuTimeLimit) and
	 * parking a higher-than-main-priority thread on it wedges Luma 13.3.2's
	 * Rosalina menu-leave into a console hard-lock (hardware bisect with
	 * tools/hello-world-3ds), so the worker never touches core 1 and no
	 * core-1 time is reserved. Core 3 first (nothing else of ours runs
	 * there); core 2 second (shared with the render worker, which runs at
	 * main priority); otherwise the game thread drives the DSP itself. Never a thread on
	 * main's own core: core_id=-1 means "all CPUs" and resolves to core 0.
	 */
	mainPriority = 0x30;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	audioPriority = mainPriority - 1;
	if (audioPriority < 0x19)
	{
		audioPriority = 0x19;
	}
	if (audioPriority > 0x2F)
	{
		audioPriority = 0x2F;
	}
	/* Clamp mirrors SDL_n3dsaudio.c's real audio-thread convention: 0x18 is
	 * reserved for video, 0x30 is the default for the main thread. Higher
	 * priority than main is fine here only because the worker is always on
	 * a core distinct from main's. */

	audioCore = 3;
	s_audioThread = threadCreate(N3DS_AudioThreadMain, NULL, 32 * 1024, audioPriority, audioCore, false);
	if (s_audioThread == NULL)
	{
		audioCore = 2;
		s_audioThread = threadCreate(N3DS_AudioThreadMain, NULL, 32 * 1024, audioPriority, audioCore, false);
	}

	/* threadCreate returns NULL on failure (thread.h) -- a clean failure
	 * mode, not a crash. If this happens, the caller falls back to
	 * synchronous rendering, same as the checks above. */
	s_audioThreadRunning = (s_audioThread != NULL);
	if (s_audioThreadRunning)
	{
		char msg[64];

		snprintf(msg, sizeof(msg), "[CTR-3DS] audio worker: UP (core %d)", audioCore);
		N3DS_DebugLog(msg);
	}
	else
	{
		N3DS_DebugLog("[CTR-3DS] audio worker: NO (threadCreate failed on cores 3 and 2) -- game thread drives the DSP");
	}
}

void N3DS_AudioThreadNotifyVBlank(void)
{
	LightLock_Lock(&s_audioCounterLock);
	if ((s_targetVBlanks - s_renderedVBlanks) < NATIVE_AUDIO_THREAD_MAX_BACKLOG_VBLANKS)
	{
		s_targetVBlanks++;
	}
	else
	{
		s_droppedVBlanks++; /* non-silent cap -- see design doc's "XA lag question" */
	}
	LightLock_Unlock(&s_audioCounterLock);
	svcSignalEvent(s_audioWakeHandle);
}

void N3DS_AudioThreadStopAndJoin(void)
{
	if (s_audioThreadRunning && (s_audioThread != NULL))
	{
		s_audioThreadShouldStop = 1;
		svcSignalEvent(s_audioWakeHandle);
		threadJoin(s_audioThread, U64_MAX); /* documented infinite-wait sentinel */
		threadFree(s_audioThread);          /* valid only because detached=false at creation */
		s_audioThread = NULL;
		s_audioThreadRunning = 0;
	}
	/*
	 * Wake handle and ndsp sink are deliberately left up: N3DS_AudioThreadIsRunning()
	 * now reads false, so NativeAudio_StepVBlank drives the same sink from the
	 * game thread on the very next vblank (see that function's 3DS branch) -- exactly the path already exercised whenever Init never
	 * secured a worker in the first place. One-way: nothing here restarts the
	 * worker.
	 */
}

void N3DS_AudioThreadShutdown(void)
{
	N3DS_AudioThreadStopAndJoin();
	if (s_audioWakeHandle != 0)
	{
		svcCloseHandle(s_audioWakeHandle);
		s_audioWakeHandle = 0;
	}

	/* After the join (or if no worker ever existed): nothing can be
	 * submitting anymore, from any thread -- safe to tear the sink down.
	 * No-op if the sink never came up. */
	N3DS_NdspSinkShutdown();
}

int N3DS_AudioThreadIsRunning(void)
{
	return s_audioThreadRunning;
}

void N3DS_NdspForceOff(void)
{
	N3DS_AudioThreadShutdown();
	/* Shutdown clears s_ndspInitAttempted so a later re-Init can normally
	 * bring the sink back; this latch keeps N3DS_NdspSinkInit refusing even
	 * though NativeAudio_SpuInit calls N3DS_AudioThreadInit on every XA play
	 * request. */
	s_ndspForcedOff = 1;
}

int N3DS_AudioThreadGetBacklog(void)
{
	int backlog;

	LightLock_Lock(&s_audioCounterLock);
	backlog = (int)(s_targetVBlanks - s_renderedVBlanks);
	LightLock_Unlock(&s_audioCounterLock);

	return backlog;
}

int N3DS_AudioThreadGetDropped(void)
{
	int dropped;

	LightLock_Lock(&s_audioCounterLock);
	dropped = (int)s_droppedVBlanks;
	LightLock_Unlock(&s_audioCounterLock);

	return dropped;
}

static void N3DS_AudioRenderLockTimed(void)
{
	u64 mark = svcGetSystemTick();

	LightLock_Lock(&s_audioRenderLock);
	s_profLockWaitTicks += svcGetSystemTick() - mark;
}

void N3DS_AudioLock(void)
{
	/* threadGetCurrent() is NULL on the main thread and the worker's own
	 * Thread handle on the worker, so this attributes the wait without a
	 * thread-local or an extra syscall on the common path. */
	if ((s_audioThread != NULL) && (threadGetCurrent() == s_audioThread))
	{
		N3DS_AudioRenderLockTimed();
		return;
	}

	LightLock_Lock(&s_audioRenderLock);
}

void N3DS_AudioUnlock(void)
{
	LightLock_Unlock(&s_audioRenderLock);
}
