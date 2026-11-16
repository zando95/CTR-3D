#include <macros.h>
#include <platform/native_audio.h>
#include <platform/native_assets.h>
#include <platform/native_disc_image.h>
#include <platform/native_path.h>
#include <platform/native_perf.h>

#if !defined(__3DS__)
#include <SDL3/SDL.h>
#else
#include "n3ds/n3ds_audio_thread.h"
#endif
#include <limits.h>
#include <stdarg.h> /* NativeAudio_XAVoiceCacheLog (3DS SD-log sink) */
#include <stddef.h> /* offsetof -- NativeAudio_XAStreamSwap's partial swap */
#include <stdio.h>
#include <stdlib.h>
#include <time.h> /* clock() -- NativeAudio_LifecycleBench only */
#include <string.h>

/*
 * Below this the idle-arena reclaim leaves decoded voice PCM alone, so a
 * re-triggered sound between-sounds re-keys for free instead of paying a
 * full re-decode. With the
 * exact-size reservations of step 2 a normal SFX working set sits well under
 * this, so the unbounded-growth defence the reclaim was added for only has
 * to fire in genuinely pathological sessions; the bank-reload path
 * (NativeAudio_ResetVoicePcmArena) still drops everything unconditionally.
 */
#define NATIVE_AUDIO_VOICE_ARENA_RECLAIM_KEEP (256 * 1024)

#if defined(__3DS__)
/*
 * Hardware voices (plan B; see n3ds_audio_thread.h): decoded
 * PCM lives in the DSP-readable linear store, decoded ONCE per (addr,
 * loop_addr) instead of per key-on -- SpuWrite already invalidates
 * everything through NativeAudio_ResetVoicePcmArenaNoLock, which is the
 * single reset hook for this cache too. Entry count: a full SPU bank is
 * dozens of samples; 128 is generous and the table resets on every upload.
 *
 * The mirror shadows are deliberately OUTSIDE struct NativeAudioVoice:
 * that struct is captured by save states, and these are per-boot DSP
 * bookkeeping, not audio state.
 */
#define NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES 128

/*
 * Chunked first-play decode. The head chunk is what a key-on pays on the game
 * thread; it must outlast the DSP's worst-case read (4x pitch = 176.4k
 * samples/s) until the audio thread's next wake (~8ms), with margin: 8192
 * samples covers ~46ms at 4x. The pump budget per wake (~2 wakes/frame) gives
 * ~1M samples/s of fill throughput -- more than 5x ahead of worst-case
 * consumption -- in chunk-sized lock holds so a game-thread key-on never
 * waits on more than one chunk.
 */
#define NATIVE_AUDIO_HW_DECODE_HEAD_SAMPLES 8192
/* With no audio thread the pump runs once per vblank the game emits, and those
 * arrive in a burst per rendered frame, so the head must outlast a slow frame:
 * 16384 samples is ~93ms at 4x pitch, ~370ms at 1x. */
#define NATIVE_AUDIO_HW_DECODE_HEAD_SAMPLES_INLINE 16384
#define NATIVE_AUDIO_HW_DECODE_CHUNK_SAMPLES 4096
#define NATIVE_AUDIO_HW_DECODE_PUMP_SAMPLES 16384

/*
 * Keyed on the SPU address alone. NativeAudio_DecodeSpuSample and the extent
 * scan both walk from addr to the first ADPCM_LOOP_END, so the decoded PCM
 * and its length are a function of addr and nothing else; a voice's loop
 * address only shifts loopStart, which every key-on reads from its own scan.
 * Widening the key would admit the same decoded sample into a second slot.
 */
struct NativeAudioHwSampleCacheEntry
{
	u32 addr;
	s16 *pcm;
	int capacity; /* slot size in samples -- slots outlive their content */
	int count;
	u32 spuBytes; /* SPU-side extent (16 bytes per 28 samples), for overlap tests */
	u32 lastUse;  /* claim/hit ordinal -- the reclaim gives up the smallest first */
	int used;
	int stale; /* slot allocated but content invalidated by an SpuWrite */
	/* Chunked-decode cursor: key-on decodes only a head chunk on the game
	 * thread and the audio thread pumps the rest, so `decoded < count` means
	 * the tail is still all-zero (the slot is zero-filled first -- an overrun
	 * reads silence, never recycled PCM). The ADPCM predictor pair lives here
	 * so the block walk can resume across calls. */
	int decoded;
	u32 nextBlock;
	int histOld;
	int histOlder;
};

global_variable struct NativeAudioHwSampleCacheEntry s_hwSampleCache[NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES];

/* Monotonic use ordinal stamped on every cache hit and every claim. The store
 * runs full once a bank's whole working set is decoded, so which entry the
 * reclaim gives up decides whether a hot sample has to be decoded twice. */
global_variable u32 s_hwSampleUseSeq;

/* Key-ons the store could not serve at all -- the sound is lost. Reported as
 * storeLost on the audio shape line; the store's own allocation-failure count
 * (storeFull) includes attempts the reclaim below went on to satisfy. */
global_variable u32 s_hwStoreLostKeyOns;

#ifdef CTR_INTERNAL
/*
 * Store-growth census, the instrument for any future "the store is full"
 * hunt: allocs/allocBytes are what the store was asked to hold, releases and
 * reclaimRetries how much of it came back. dupAddr is a tripwire -- a fresh
 * slot taken for an address the table already covers means the cache key has
 * stopped being the SPU address alone. growReleases is the streaming case:
 * the same address re-scanned larger than its slot, whose old block would
 * otherwise be stranded.
 */
struct NativeAudioHwStoreCensus
{
	u32 allocs;
	u32 allocBytes;
	u32 dupAddr;
	u32 dupBytes;
	u32 growReleases;
	u32 releases;
	u32 reclaimRetries;
};
global_variable struct NativeAudioHwStoreCensus s_hwStoreCensus;
#endif

struct NativeAudioHwVoiceShadow
{
	int keyonPending;   /* set at SpuSetKey(on), consumed by the mirror */
	int started;        /* a DSP channel is currently playing this voice */
	u16 lastPitch;      /* SetRate pokes only on change */
	float lastGainL;    /* SetMix pokes only on change: every ndspChn* call
	                       crosses ndsp's command mutex, and 23 unconditional
	                       pokes per vblank showed up as worker lag
	                       (audioDropped creep) in hardware run 2. Gains are
	                       exact float products of integers, so equality
	                       compare is sound. */
	float lastGainR;
};

global_variable struct NativeAudioHwVoiceShadow s_hwVoiceShadow[24];

/*
 * Where the audio worker's per-wake budget actually goes. Hardware run 6
 * left the session with a verdict but not a breakdown: on an XA-music track
 * the worker is the frame-rate governor (audioDropped 7,661, ndspQ pinned at
 * 0 in-race), but "worker over budget" covers five different jobs sharing
 * one loop. These split it -- the two halves timed here, the rest in
 * n3ds_audio_thread.c, all reported together on the audio shape line -- so
 * the next hardware log names the dominant cost instead of us inferring it.
 *
 * Written only by the mixing thread (worker, or the StepVBlank fallback --
 * never both, same invariant as the sink ring). Read unlocked for reporting,
 * which is the established precedent for this class of counter.
 */
struct NativeAudioWorkerProfile
{
	unsigned long long lifecycleTicks;
	unsigned long long mirrorTicks;
};

global_variable struct NativeAudioWorkerProfile s_workerProfile;
#endif


#if CTR_NATIVE_AUDIO_DECODE_PROFILE
/*
 * Voice-start decode counters.
 * Window-scoped ones reset when the [PERF-VOICE] line is emitted; frameKeyOns
 * is consumed by native_perf.c once per frame for the worst-frame/key-on
 * correlation test. Same unity TU, so `internal` linkage crosses the files.
 */
global_variable struct
{
	int windowKeyOns;
	int windowDecodeCalls;
	int windowDecodedSamples;
	int windowScannedBlocks;
	int windowArenaGrows;
	int arenaPeakKiB;
	int frameKeyOns;
	/* HW sample-cache lifecycle, to attribute a decode storm to its trigger:
	 * full resets vs. entries staled by SpuWrite vs. in-place stream
	 * refreshes (each refresh is itself a decode), plus the write volume
	 * that drove the invalidation. 3DS-only paths feed these; they print as
	 * zeros on PC. */
	int windowHwResets;
	int windowHwStaled;
	int windowHwRefresh;
	int windowSpuWriteBytes;
} s_voiceProf;
#endif

#define NATIVE_AUDIO_SAMPLE_RATE            44100
#define NATIVE_AUDIO_CHANNELS               2
#define NATIVE_AUDIO_SPU_VOICE_COUNT        24
#define NATIVE_AUDIO_SPU_MEMSIZE            (512 * 1024)
#define NATIVE_AUDIO_FP_SHIFT               16
#define NATIVE_AUDIO_FP_ONE                 (1 << NATIVE_AUDIO_FP_SHIFT)
#define NATIVE_AUDIO_GAUSS_INDEX_SHIFT      8
// Matches PSX SPU Q15 direct-volume semantics: each volume register (voice
// VOLL/VOLR, CD CDVOLL/CDVOLR, master MVOLL/MVOR) is a signed 16-bit value
// with effective gain = value / 0x8000, and two stages (per-voice then
// master) divide by 0x8000^2 = 0x40000000. Retail HOWL ships vol_Music=175,
// vol_FX=215, vol_Voice=255, master=0x3fff, and Channel_SetVolume caps each
// voice at 0x3fff; this divisor reproduces PSX hardware amplitude at those
// defaults (e.g. 0x3fff^2 / 0x40000000 ~= 0.25 per voice).
#define NATIVE_AUDIO_DIRECT_VOL_MAX         0x8000
#define NATIVE_AUDIO_VBLANK_FRAMES          (NATIVE_AUDIO_SAMPLE_RATE / 60)
#define NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES (NATIVE_AUDIO_VBLANK_FRAMES * 16)
#define NATIVE_AUDIO_XA_ZIGZAG_TAPS         29
#define NATIVE_AUDIO_XA_ZIGZAG_PHASES       7
#define NATIVE_AUDIO_XA_ZIGZAG_INPUTS       6
// NOTE(aalhendi): Little-endian tag `CTRA` = CTR native Audio snapshot.
#define NATIVE_AUDIO_STATE_MAGIC            0x41525443
#define NATIVE_AUDIO_STATE_VERSION          1
#define NATIVE_AUDIO_ARENA_ALIGN            16
#define NATIVE_AUDIO_ADSR_MIN               (-0x8000)
#define NATIVE_AUDIO_ADSR_MAX               0x7fff
#define NATIVE_AUDIO_ADSR_STEP_BIT          0x8000u
#define NATIVE_AUDIO_REVERB_MAX_BYTES       0x18040
#define NATIVE_AUDIO_REVERB_MAX_SAMPLES     (NATIVE_AUDIO_REVERB_MAX_BYTES / (int)sizeof(s16))
#define NATIVE_AUDIO_REVERB_FIR_TAPS        39

#define XA_NUM_TYPES                        3
#define XA_HEADER_SIZE                      0x44
#define XA_NUM_XAS_TOTAL_OFFSET             0x0c
#define XA_NUM_TRACKS_TOTAL_OFFSET          0x10
#define XA_NUM_SONGS_OFFSET                 0x2c
#define XA_FIRST_SONG_INDEX_OFFSET          0x38
#define XA_SIZE_ENTRY_BYTES                 4
#define XA_FORM2_SECTOR_SIZE                2336
#define XA_FULL_SECTOR_SIZE                 2352
#define XA_FRAMES_PER_SECTOR                18
#define XA_FRAME_SIZE                       128
#define XA_SUBHEADER_SIZE                   8
#define XA_SAMPLES_PER_SOUND_UNIT           28
#define XA_BLOCKS_PER_FRAME                 4
#define XA_SUBFRAMES_PER_FRAME              8
#define XA_SAMPLE_RATE_37800                37800
#define XA_SAMPLE_RATE_18900                18900

// Streaming ring for decoded XA PCM. One physical sector's worth of ring-slot
// capacity: 18*8*28 = 4032, the MONO decode count (DecodeXASectorMono pushes
// 18*8*28 raw mono samples, which after L=R duplication become 4032 output
// frames). A STEREO sector only fills the first 2016 of those 4032 slots
// (DecodeXASectorStereo already interleaves L/R while decoding: 18*4*28 =
// 2016 output frames). Size the slot for the mono (larger) case and pin the
// real per-track stride in struct NativeAudioXaStream.framesPerSector -- a
// single hardcoded stride would corrupt stereo frameIndex<->sector math.
#define NATIVE_AUDIO_XA_SECTOR_FRAMES      4032  // 18*8*28, one ring slot's worst-case (mono) capacity in stereo output frames
#if defined(__3DS__)
/*
 * 3DS ring is much deeper than PC: 6 sectors is only ~0.3s of cushion, and
 * the worker's refill reads share the SD card (and its FAT mutex) with bulk
 * scene loads that can monopolize it for whole seconds. 32+ sectors rides
 * out those bursts; the memory (~2MB across both stream buffers) is cheap
 * now that the decoded-voice arena is gone. The synchronous prime at track
 * open stays SMALL (NATIVE_AUDIO_XA_PRIME_TARGET) so opens do not stall the
 * main thread; the worker's burst pump tops the rest up. PC must keep the
 * original ring geometry: replay determinism depends on ring
 * residency/underrun timing staying exactly as it was.
 */
#define NATIVE_AUDIO_XA_RING_SECTORS       64
#define NATIVE_AUDIO_XA_RING_AHEAD_TARGET  56
#define NATIVE_AUDIO_XA_PRIME_TARGET       8
/*
 * Span read-ahead. XA channels are interleaved on disc, so fetching ONE audio
 * sector means up to 8+ separate 2.3KB reads (most discarded as other
 * channels), each crossing the FAT driver's global mutex against main-thread
 * bulk loads -- ~150 tiny reads/sec for one music stream. This layer replaces
 * them with one 32-sector (~75KB) sequential read cached per stream; the
 * decode loop then consumes physical sectors from memory, cutting mutex
 * crossings by ~32x with each crossing doing useful work. The deeper ring
 * above is the cushion that rides out load-burst stalls the span reads
 * cannot avoid entirely.
 */
/*
 * Tracks the disc reader's chunk size: a span fill is one read either way,
 * and an SD read costs ~26ms fixed + ~0.24ms/sector, so covering twice the
 * sectors per fill costs ~15ms more on a call that already costs ~45ms --
 * half the calls for a fifth more time each. The single blocking read the
 * worker sits inside grows with it, which is why this tracks the chunk
 * rather than exceeding it: the DSP cushion is ~200ms and a ~52ms fill sits
 * comfortably under that.
 */
#define NATIVE_AUDIO_XA_SPAN_SECTORS       64
/*
 * With no audio thread the read and the decode run on the game thread, so a
 * stall is a visible frame. An SD read on the Old 3DS costs about 4.2 ms per
 * call plus 0.64 ms a sector, so reads stay 16 sectors: smaller ones pay the
 * per-call cost more often for the same bytes. The span buffer is a ring
 * (NativeAudio_XASpanRingAppend) the pump tops up with one 16-sector read when
 * fewer than 16 sectors are left ahead of the decode, about once every 6
 * vblanks of playback (one sector in 16 for a mono voice line, one in 8 for a
 * stereo sting). A sector the ring does not hold yet is read synchronously,
 * the same 16 at a time.
 *
 * A stream starts with one decoded sector instead of the prime depth: the
 * parcel top-up waits for decoded data instead of playing past it
 * (NativeAudio_RenderXAParcelForAudioThread), and the pump decodes the rest
 * one sector per vblank from what the ring already holds, reading
 * synchronously only when the music channel is about to run dry.
 */
#define NATIVE_AUDIO_XA_SPAN_SECTORS_INLINE      16
#define NATIVE_AUDIO_XA_READAHEAD_SECTORS_INLINE 16
#define NATIVE_AUDIO_XA_AHEAD_TARGET_INLINE      NATIVE_AUDIO_XA_PRIME_TARGET
#define NATIVE_AUDIO_XA_PRIME_TARGET_INLINE      1
/* Parcels left in the music channel below which the pump may block on the SD card. */
#define NATIVE_AUDIO_XA_STARVE_PARCELS_INLINE    3
#else
#define NATIVE_AUDIO_XA_RING_SECTORS       6     // resident sector-slots per stream (~94.5 KiB @ mono capacity)
#define NATIVE_AUDIO_XA_RING_AHEAD_TARGET  4     // refill ceiling, sectors kept ahead of playback
#define NATIVE_AUDIO_XA_PRIME_TARGET       NATIVE_AUDIO_XA_RING_AHEAD_TARGET
#endif

enum
{
	ADPCM_LOOP_END = 1 << 0,
	ADPCM_REPEAT = 1 << 1,
	ADPCM_LOOP_START = 1 << 2
};

enum NativeAudioAdsrPhase
{
	NATIVE_AUDIO_ADSR_OFF,
	NATIVE_AUDIO_ADSR_ATTACK,
	NATIVE_AUDIO_ADSR_DECAY,
	NATIVE_AUDIO_ADSR_SUSTAIN,
	NATIVE_AUDIO_ADSR_RELEASE
};

enum NativeAudioReverbRegister
{
	NATIVE_AUDIO_REV_DAPF1,
	NATIVE_AUDIO_REV_DAPF2,
	NATIVE_AUDIO_REV_VIIR,
	NATIVE_AUDIO_REV_VCOMB1,
	NATIVE_AUDIO_REV_VCOMB2,
	NATIVE_AUDIO_REV_VCOMB3,
	NATIVE_AUDIO_REV_VCOMB4,
	NATIVE_AUDIO_REV_VWALL,
	NATIVE_AUDIO_REV_VAPF1,
	NATIVE_AUDIO_REV_VAPF2,
	NATIVE_AUDIO_REV_MLSAME,
	NATIVE_AUDIO_REV_MRSAME,
	NATIVE_AUDIO_REV_MLCOMB1,
	NATIVE_AUDIO_REV_MRCOMB1,
	NATIVE_AUDIO_REV_MLCOMB2,
	NATIVE_AUDIO_REV_MRCOMB2,
	NATIVE_AUDIO_REV_DLSAME,
	NATIVE_AUDIO_REV_DRSAME,
	NATIVE_AUDIO_REV_MLDIFF,
	NATIVE_AUDIO_REV_MRDIFF,
	NATIVE_AUDIO_REV_MLCOMB3,
	NATIVE_AUDIO_REV_MRCOMB3,
	NATIVE_AUDIO_REV_MLCOMB4,
	NATIVE_AUDIO_REV_MRCOMB4,
	NATIVE_AUDIO_REV_DLDIFF,
	NATIVE_AUDIO_REV_DRDIFF,
	NATIVE_AUDIO_REV_MLAPF1,
	NATIVE_AUDIO_REV_MRAPF1,
	NATIVE_AUDIO_REV_MLAPF2,
	NATIVE_AUDIO_REV_MRAPF2,
	NATIVE_AUDIO_REV_VLIN,
	NATIVE_AUDIO_REV_VRIN,
	NATIVE_AUDIO_REV_REG_COUNT
};

struct NativeAudioReverbPreset
{
	s32 mode;
	s32 sizeBytes;
	s16 reg[NATIVE_AUDIO_REV_REG_COUNT];
};

struct NativeAudioReverbState
{
	s16 buffer[NATIVE_AUDIO_REVERB_MAX_SAMPLES];
	s16 inputHistoryLeft[NATIVE_AUDIO_REVERB_FIR_TAPS];
	s16 inputHistoryRight[NATIVE_AUDIO_REVERB_FIR_TAPS];
	s16 outputHistoryLeft[NATIVE_AUDIO_REVERB_FIR_TAPS];
	s16 outputHistoryRight[NATIVE_AUDIO_REVERB_FIR_TAPS];
	s32 mode;
	s32 sizeSamples;
	s32 cursor;
	s32 inputHistoryCursor;
	s32 outputHistoryCursor;
	s32 samplePhase;
	s16 lastOutLeft;
	s16 lastOutRight;
};

struct NativeAudioSpuArena
{
	// NOTE(aalhendi): Emulated PS1 SPU RAM; this is snapshot state, not host PCM.
	u8 memory[NATIVE_AUDIO_SPU_MEMSIZE];
	int allocCursor;
	int transferOffset;
};

struct NativeAudioOutput
{
	SDL_AudioDeviceID device;
	SDL_AudioStream *stream;
	b32 deterministicRenderMode;
	s16 scheduledPcm[NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES * NATIVE_AUDIO_CHANNELS];
	int scheduledReadFrame;
	int scheduledFrameCount;
#ifdef CTR_INTERNAL
	int underrunFrames;
	int overflowFrames;
	int reportVBlankCountdown;
	int lastReportedUnderrunFrames;
	int lastReportedOverflowFrames;
	int callbackMaxRequestFrames;
	int lastReportedCallbackMaxRequestFrames;
#endif
};

// Diagnostics for the decoded-voice PCM arena. This arena grows by doubling and
// is only fully reclaimed by SpuWrite (audio-bank load) or when all 24 SPU
// voices happen to be idle at once -- a condition background music prevents.
// A Time Trial "Retry" takes the LOAD_RESTART fast path, which reloads no audio
// banks, so neither reset fires and decoded PCM accumulates across retries.
// Set to 1 to log every growth step and every reset.
#ifndef CTR_NATIVE_AUDIO_ARENA_LOG
#define CTR_NATIVE_AUDIO_ARENA_LOG 0
#endif

struct NativeAudioDecodeArena
{
	// NOTE(aalhendi): Native rebuildable cache memory. Do not snapshot this;
	// save SPU RAM, XA identity, and playback cursors, then rebuild decoded PCM on restore.
	u8 *memory;
	int capacity;
	int used;
};

struct NativeAudioVoice
{
	SpuVoiceAttr attr;
	// NOTE(aalhendi): Points into voicePcmArena; the source of truth is compressed SPU RAM at attr.addr.
	s16 *pcm;
	int sampleCount;
	int loopStart;
	int loopEnd;
	b32 loopEnabled;
	b32 active;
	// SPU-backed long samples can exceed the 65,535-frame range of u32 16.16.
	u64 positionFp;
	b32 looped;
	u16 reverb;
	b32 sampleDirty;
	s32 adsrLevel;
	u32 adsrCounter;
	u8 adsrPhase;
};

struct NativeAudioXaDecodeState
{
	int old[2];
	int older[2];
};

// NOTE(aalhendi): Resolved once per track-open; disc-image OR host-file
// backed. Mirrors struct NativeCDOpenFile's tag-plus-one-member-per-backing
// idiom (platform/native_cd.c), the established pattern in this codebase for
// "open from host file, else fall back to the disc image" resources.
struct NativeAudioXaSource
{
	b32 useDiscImage;
	struct NativeDiscImageFile discFile;   // valid iff useDiscImage; via NativeDiscImage_FindFile
	struct NativeAssetsHostRange hostFile; // valid iff !useDiscImage
	int sectorSize, sectorBase, totalSectors, channelFilter;
#if defined(__3DS__)
	/* Voice-cache RAM backing (see the XA voice cache section). Non-NULL wins
	 * over both file backends: every sector read is a memcpy from this blob,
	 * indexed 0..totalSectors-1 at sectorSize stride. The worker pump reads
	 * the blob without locking: only a rebuild rewrites it, and the rebuild
	 * stops a stream backed by it before its first write, which lands after a
	 * whole XA file has been read from the card. */
	const u8 *ramSectors;
#endif
};

// NOTE(aalhendi): Fixed-size streaming ring for one XA track's decoded PCM.
// Replaces the old ever-growing xaPcmArena/xaPendingPcmArena. ringPcm points at a static backing array
// (not inline -- see s_xaRingPcmLive/s_xaRingPcmPending), assigned once,
// lazily, by NativeAudio_XAStreamInitRingBackingIfNeeded.
//
// framesPerSector and decodedSectorCount are deviations from the plan's
// literal struct shape, both required for correctness (see session report):
//   - framesPerSector: a physical sector decodes to NATIVE_AUDIO_XA_SECTOR_FRAMES
//     (4032) output frames if the source is mono, but only half that (2016)
//     if stereo (DecodeXASectorStereo already interleaves L/R while
//     decoding). This is pinned from the first matching sector, same as
//     sampleRate, and used for all frameIndex<->sector arithmetic instead of
//     the compile-time constant.
//   - decodedSectorCount: a logical count of sectors actually decoded into
//     the ring so far, distinct from nextPhysicalSector (the raw read/scan
//     cursor into the source, which also advances across skipped non-audio
//     sectors in multiplexed files, e.g. TEST.STR's interleaved MDEC video).
//     Ring-slot assignment and frameIndex math must use the logical count,
//     not the physical cursor, or playback desyncs from the ring on any file
//     with non-audio sectors interspersed.
struct NativeAudioXaStream
{
	s16 *ringPcm;
	/* Span read-ahead cache (3DS; see NATIVE_AUDIO_XA_SPAN_SECTORS). Backing
	 * array is global like ringPcm's, assigned by the same lazy init,
	 * preserved across ClearNoLock the same way -- and the clear's memset
	 * zeroing spanSectorCount is exactly the invalidation every open/seek
	 * path needs, since they all Clear first. Fields sit BEFORE sectorBuf so
	 * XAStreamSwap carries them with their stream: the cached bytes belong
	 * to this stream's source and must trade places when the streams do.
	 * Unused (never read, stays zero) on PC. */
	u8 *spanBuf;
	/* Disc-read staging for this stream's reads; travels with the stream for
	 * the same reason spanBuf does. Unused (stays NULL) on PC. */
	struct NativeDiscImageScratch *discScratch;
	int spanStartSector;
	int spanSectorCount;
	struct NativeAudioXaSource source;
	struct NativeAudioXaDecodeState decodeState; // ADPCM predictor history; persists sector-to-sector
	int nextPhysicalSector;
	int decodedSectorCount;
	int sampleRate;      // pinned from first matching sector
	int framesPerSector; // pinned from first matching sector: 2016 (stereo) or 4032 (mono)
	b32 open, endOfStream;
	u32 underrunEvents; // diagnostic counter, see stats update in native_renderer_3ds.c
	/* Per-stream decode scratch. These were function-local statics in
	 * NativeAudio_XAStreamDecodeNextSector for years -- safe while only the
	 * main thread ever decoded. On 3DS the worker pumps the LIVE stream
	 * while the main thread primes the PENDING one (PlayXATrack at e.g. the
	 * race-start jingle), and session-20 hardware run 5 hard-wedged the
	 * console right at that overlap: two threads trampling shared scratch
	 * mid-decode. Per-stream buffers make the two concurrent decodes
	 * disjoint; same-stream use stays single-threaded by the existing
	 * design (the pump owns live; the Open and seek paths own pending). */
	u8 sectorBuf[XA_FULL_SECTOR_SIZE];
	s16 tempFrames[NATIVE_AUDIO_XA_SECTOR_FRAMES * NATIVE_AUDIO_CHANNELS];
};

internal int NativeAudio_XARingReadSampleNoLock(struct NativeAudioXaStream *stream, int channel, u64 frameIndex);
internal void NativeAudio_XAStreamClearNoLock(struct NativeAudioXaStream *stream);

struct NativeAudioXA
{
	int frameCount;
	int sampleRate;
	int categoryID;
	int xaID;
	b32 hasTrackIdentity;
	b32 active;
	// XA streams can exceed the 65,535-frame range of a u32 16.16 cursor.
	u64 positionFp;
	u64 outputFrame;
	u32 stepFp;
	s16 volumeLeft;
	s16 volumeRight;
};

struct NativeAudioState
{
	b32 init;
	b32 muted;
	b32 reverbEnabled;
	b32 cdMixEnabled;
	b32 cdReverbEnabled;
	b32 reverbWorkAreaReserved;
	s16 masterVolumeLeft;
	s16 masterVolumeRight;
	u32 reverbVoiceBits;
	SpuReverbAttr reverbAttr;
	struct NativeAudioReverbState reverb;
	SpuCommonAttr commonAttr;
	struct NativeAudioSpuArena spu;
	struct NativeAudioDecodeArena voicePcmArena;
	struct NativeAudioVoice voices[NATIVE_AUDIO_SPU_VOICE_COUNT];
	struct NativeAudioXA xa;
	struct NativeAudioXaStream xaStream;
	struct NativeAudioXaStream xaPendingStream;
	struct NativeAudioOutput output;
};

struct NativeAudioByteBuffer
{
	u8 *data;
	int size;
};

struct NativeAudioPcmBuffer
{
	s16 *samples;
	int count;
	int capacity;
};

struct NativeAudioXaTrackInfo
{
	int fileNumber;
	int channelFilter;
	int numSectors;
	int entryIndex; // XNF-global track index (firstSongIndex[category] + xaID); keys the voice cache + count memo
	int pack;       // 0 = the disc's own (NTSC-U English) voices; 1..6 = a PAL voice pack (3DS). entryIndex is then offset by pack * 512 so the keys never collide
};

struct NativeAudioVoiceState
{
	SpuVoiceAttr attr;
	int sampleCount;
	int loopStart;
	int loopEnd;
	b32 loopEnabled;
	b32 active;
	u64 positionFp;
	b32 looped;
	u16 reverb;
	b32 sampleDirty;
	s32 adsrLevel;
	u32 adsrCounter;
	u8 adsrPhase;
};

struct NativeAudioXAState
{
	int frameCount;
	int sampleRate;
	int categoryID;
	int xaID;
	b32 hasTrackIdentity;
	b32 active;
	u64 positionFp;
	u64 outputFrame;
	u32 stepFp;
	s16 volumeLeft;
	s16 volumeRight;
};

struct NativeAudioSnapshot
{
	u32 magic;
	u32 version;
	u32 size;
	b32 init;
	b32 muted;
	int spuAllocCursor;
	b32 reverbEnabled;
	b32 cdMixEnabled;
	b32 cdReverbEnabled;
	b32 reverbWorkAreaReserved;
	int spuTransferOffset;
	s16 masterVolumeLeft;
	s16 masterVolumeRight;
	u32 reverbVoiceBits;
	SpuReverbAttr reverbAttr;
	struct NativeAudioReverbState reverb;
	SpuCommonAttr commonAttr;
	struct NativeAudioVoiceState voices[NATIVE_AUDIO_SPU_VOICE_COUNT];
	struct NativeAudioXAState xa;
	u8 spuSampleMem[NATIVE_AUDIO_SPU_MEMSIZE];
};

global_variable struct NativeAudioState s_audio;

// NOTE(aalhendi): Fixed backing storage for the two XA streaming rings (live +
// pending). Not stack-allocated: no confirmed 3DS thread stack size for this
// project, and this matches how the SPU sample memory arena is already
// embedded as a static-lifetime member of s_audio. ~94.5 KiB each, ~189 KiB
// total, constant for the process lifetime -- versus the old arenas' measured
// 32 MiB and climbing.
global_variable s16 s_xaRingPcmLive[NATIVE_AUDIO_XA_RING_SECTORS * NATIVE_AUDIO_XA_SECTOR_FRAMES * NATIVE_AUDIO_CHANNELS];
global_variable s16 s_xaRingPcmPending[NATIVE_AUDIO_XA_RING_SECTORS * NATIVE_AUDIO_XA_SECTOR_FRAMES * NATIVE_AUDIO_CHANNELS];
#if defined(__3DS__)
/* Span read-ahead backing (one per stream, ~74KB each) -- global for the
 * same reason the ring arrays above are: keeping big buffers out of the
 * stream struct is what keeps XAStreamSwap's temp (and every future
 * by-value use) off the 32KB thread stacks (session-20 lesson). */
global_variable u8 s_xaSpanBufLive[NATIVE_AUDIO_XA_SPAN_SECTORS * XA_FULL_SECTOR_SIZE];
global_variable u8 s_xaSpanBufPending[NATIVE_AUDIO_XA_SPAN_SECTORS * XA_FULL_SECTOR_SIZE];
#endif
/* Disc-image staging, same live/pending split and the same reason: the disc
 * reader batches a run of raw sectors through a caller-owned scratch, and on
 * 3DS the worker owns the live stream while the main thread owns the pending
 * one, so one scratch per stream keeps the two threads off each other's
 * buffer without a lock. Not 3DS-gated, unlike the span buffers above: PC
 * reads XA one sector per vblank and wants no span, but tagging those reads
 * with the audio ROLE is what lets [DISCIO] separate the music stream's drip
 * from the level loader's bursts on either platform.
 *
 * Pending stays tagged MAIN on purpose: a pending stream is filled by the
 * track-open path, which runs on the main thread on 3DS. Tagging it AUDIO
 * would read as worker time in the counter and blur the one split the 3DS
 * contention question actually needs.
 *
 * Owners are assigned at init, not in the initialiser: a non-zero initialiser
 * puts the whole 147KB in .data, where it is 147KB of zeros carried in the
 * binary (exactly the size the 3dsx grew by when the chunk went to 64). Left
 * uninitialised they land in .bss. */
global_variable struct NativeDiscImageScratch s_xaDiscScratchLive;
global_variable struct NativeDiscImageScratch s_xaDiscScratchPending;

internal b32 NativeAudio_OutputOpen(void)
{
	return s_audio.output.stream != NULL;
}

internal void NativeAudio_LockOutput(void)
{
	if (s_audio.output.stream != NULL)
	{
		SDL_LockAudioStream(s_audio.output.stream);
	}
}

internal void NativeAudio_UnlockOutput(void)
{
	if (s_audio.output.stream != NULL)
	{
		SDL_UnlockAudioStream(s_audio.output.stream);
	}
}

global_variable const int s_posTable[5] = {0, 60, 115, 98, 122};
global_variable const int s_negTable[5] = {0, 0, -52, -55, -60};
// NOTE(aalhendi): PS1 SPU Gaussian interpolation coefficient table.
// Source reference: https://psx-spx.consoledev.net/soundprocessingunitspu/
global_variable const s16 s_gaussTable[512] = {
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    0,     0,     0,     0,     0,     0,
    0,     1,     1,     1,     1,     2,     2,     2,     3,     3,     3,     4,     4,     5,     5,     6,     7,     7,     8,     9,     9,     10,
    11,    12,    13,    14,    15,    16,    17,    18,    19,    21,    22,    24,    25,    27,    28,    30,    32,    33,    35,    37,    39,    41,
    44,    46,    48,    51,    53,    56,    58,    61,    64,    67,    70,    73,    77,    80,    84,    87,    91,    95,    99,    103,   107,   111,
    116,   120,   125,   130,   135,   140,   145,   150,   156,   161,   167,   173,   179,   186,   192,   199,   205,   212,   219,   227,   234,   242,
    250,   257,   266,   274,   283,   291,   300,   309,   319,   328,   338,   348,   358,   369,   379,   390,   401,   412,   424,   436,   448,   460,
    473,   485,   498,   512,   525,   539,   553,   567,   582,   597,   612,   627,   643,   659,   675,   692,   708,   726,   743,   761,   779,   797,
    816,   835,   854,   874,   894,   914,   935,   956,   977,   999,   1020,  1043,  1066,  1089,  1112,  1136,  1160,  1184,  1209,  1234,  1260,  1286,
    1312,  1339,  1366,  1394,  1422,  1450,  1479,  1508,  1537,  1567,  1598,  1628,  1660,  1691,  1723,  1756,  1789,  1822,  1856,  1890,  1924,  1959,
    1995,  2031,  2067,  2104,  2141,  2179,  2217,  2256,  2295,  2334,  2374,  2415,  2456,  2497,  2539,  2582,  2624,  2668,  2712,  2756,  2801,  2846,
    2892,  2938,  2985,  3032,  3079,  3128,  3176,  3225,  3275,  3325,  3376,  3427,  3479,  3531,  3584,  3637,  3691,  3745,  3799,  3855,  3910,  3967,
    4023,  4081,  4138,  4197,  4255,  4315,  4374,  4435,  4495,  4557,  4619,  4681,  4744,  4807,  4871,  4935,  5000,  5065,  5131,  5197,  5264,  5332,
    5399,  5468,  5536,  5606,  5676,  5746,  5817,  5888,  5959,  6032,  6104,  6177,  6251,  6325,  6400,  6475,  6550,  6626,  6702,  6779,  6856,  6934,
    7012,  7091,  7170,  7249,  7329,  7409,  7490,  7571,  7653,  7735,  7817,  7900,  7983,  8066,  8150,  8234,  8319,  8404,  8489,  8575,  8661,  8748,
    8834,  8922,  9009,  9097,  9185,  9273,  9362,  9451,  9541,  9630,  9720,  9811,  9901,  9992,  10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727,
    10820, 10913, 11007, 11100, 11194, 11288, 11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343, 14439, 14534, 14630, 14725, 14820, 14915,
    15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855, 15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953,
    17042, 17131, 17220, 17308, 17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665, 18746, 18826,
    18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888, 19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
    20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944, 21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706,
    21754, 21801, 21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435, 22467, 22498, 22527, 22556,
    22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826, 22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942,
    22948, 22953, 22957, 22960, 22962, 22963,
};

// NOTE(aalhendi): PS1 CD-XA 37800Hz->44100Hz zig-zag interpolation table.
// Source reference: PSX-SPX, "CDROM XA Audio ADPCM Compression".
global_variable const s16 s_xaZigZagTable[NATIVE_AUDIO_XA_ZIGZAG_TAPS][NATIVE_AUDIO_XA_ZIGZAG_PHASES] = {
    {0, 0, 0, 0, -0x0001, 0x0002, -0x0005},
    {0, 0, 0, -0x0001, 0x0003, -0x0008, 0x0011},
    {0, 0, -0x0001, 0x0003, -0x0008, 0x0010, -0x0023},
    {0, -0x0002, 0x0003, -0x0008, 0x0011, -0x0023, 0x0046},
    {0, 0, -0x0002, 0x0006, -0x0010, 0x002B, -0x0017},
    {-0x0002, 0x0003, -0x0005, 0x0005, 0x000A, 0x001A, -0x0044},
    {0x000A, -0x0013, 0x001F, -0x001B, 0x006B, -0x00EB, 0x015B},
    {-0x0022, 0x003C, -0x004A, 0x00A6, -0x016D, 0x027B, -0x0347},
    {0x0041, -0x004B, 0x00B3, -0x01A8, 0x0350, -0x0548, 0x080E},
    {-0x0054, 0x00A2, -0x0192, 0x0372, -0x0623, 0x0AFA, -0x1249},
    {0x0034, -0x00E3, 0x02B1, -0x05BF, 0x0BCD, -0x16FA, 0x3C07},
    {0x0009, 0x0132, -0x039E, 0x09B8, -0x1780, 0x53E0, 0x53E0},
    {-0x010A, -0x0043, 0x04F8, -0x11B4, 0x6794, 0x3C07, -0x16FA},
    {0x0400, -0x0267, -0x05A6, 0x74BB, 0x234C, -0x1249, 0x0AFA},
    {-0x0A78, 0x0C9D, 0x7939, 0x0C9D, -0x0A78, 0x080E, -0x0548},
    {0x234C, 0x74BB, -0x05A6, -0x0267, 0x0400, -0x0347, 0x027B},
    {0x6794, -0x11B4, 0x04F8, -0x0043, -0x010A, 0x015B, -0x00EB},
    {-0x1780, 0x09B8, -0x039E, 0x0132, 0x0009, -0x0044, 0x001A},
    {0x0BCD, -0x05BF, 0x02B1, -0x00E3, 0x0034, -0x0017, 0x002B},
    {-0x0623, 0x0372, -0x0192, 0x00A2, -0x0054, 0x0046, -0x0023},
    {0x0350, -0x01A8, 0x00B3, -0x004B, 0x0041, -0x0023, 0x0010},
    {-0x016D, 0x00A6, -0x004A, 0x003C, -0x0022, 0x0011, -0x0008},
    {0x006B, -0x001B, 0x001F, -0x0013, 0x000A, -0x0005, 0x0002},
    {0x000A, 0x0005, -0x0005, 0x0003, -0x0001, 0, 0},
    {-0x0010, 0x0006, -0x0002, 0, 0, 0, 0},
    {0x0011, -0x0008, 0x0003, -0x0002, 0x0001, 0, 0},
    {-0x0008, 0x0003, -0x0001, 0, 0, 0, 0},
    {0x0003, -0x0001, 0, 0, 0, 0, 0},
    {-0x0001, 0, 0, 0, 0, 0, 0},
};

// NOTE(aalhendi): PS1 SPU reverb 44.1kHz<->22.05kHz FIR resampler.
// Source reference: https://psx-spx.consoledev.net/soundprocessingunitspu/
global_variable const s16 s_reverbFirCoeffs[NATIVE_AUDIO_REVERB_FIR_TAPS] = {
    -0x0001, 0x0000, 0x0002, 0x0000,  -0x000A, 0x0000, 0x0023, 0x0000, -0x0067, 0x0000,  0x010A, 0x0000, -0x0268,
    0x0000,  0x0534, 0x0000, -0x0B90, 0x0000,  0x2806, 0x4000, 0x2806, 0x0000,  -0x0B90, 0x0000, 0x0534, 0x0000,
    -0x0268, 0x0000, 0x010A, 0x0000,  -0x0067, 0x0000, 0x0023, 0x0000, -0x000A, 0x0000,  0x0002, 0x0000, -0x0001,
};

#define NATIVE_AUDIO_REV(value) ((s16)(value))

// NOTE(aalhendi): PS1 SPU reverb preset registers in libspu mode order.
// Source reference: https://psx-spx.consoledev.net/soundprocessingunitspu/
global_variable const struct NativeAudioReverbPreset s_reverbPresets[] = {
    {SPU_REV_MODE_OFF, 0x10, {NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                              NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                              NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001),
                              NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001),
                              NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0000),
                              NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001),
                              NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000)}},
    {SPU_REV_MODE_ROOM, 0x26C0, {NATIVE_AUDIO_REV(0x007D), NATIVE_AUDIO_REV(0x005B), NATIVE_AUDIO_REV(0x6D80), NATIVE_AUDIO_REV(0x54B8),
                                 NATIVE_AUDIO_REV(0xBED0), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0xBA80),
                                 NATIVE_AUDIO_REV(0x5800), NATIVE_AUDIO_REV(0x5300), NATIVE_AUDIO_REV(0x04D6), NATIVE_AUDIO_REV(0x0333),
                                 NATIVE_AUDIO_REV(0x03F0), NATIVE_AUDIO_REV(0x0227), NATIVE_AUDIO_REV(0x0374), NATIVE_AUDIO_REV(0x01EF),
                                 NATIVE_AUDIO_REV(0x0334), NATIVE_AUDIO_REV(0x01B5), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                 NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                 NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x01B4), NATIVE_AUDIO_REV(0x0136),
                                 NATIVE_AUDIO_REV(0x00B8), NATIVE_AUDIO_REV(0x005C), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_STUDIO_A, 0x1F40, {NATIVE_AUDIO_REV(0x0033), NATIVE_AUDIO_REV(0x0025), NATIVE_AUDIO_REV(0x70F0), NATIVE_AUDIO_REV(0x4FA8),
                                     NATIVE_AUDIO_REV(0xBCE0), NATIVE_AUDIO_REV(0x4410), NATIVE_AUDIO_REV(0xC0F0), NATIVE_AUDIO_REV(0x9C00),
                                     NATIVE_AUDIO_REV(0x5280), NATIVE_AUDIO_REV(0x4EC0), NATIVE_AUDIO_REV(0x03E4), NATIVE_AUDIO_REV(0x031B),
                                     NATIVE_AUDIO_REV(0x03A4), NATIVE_AUDIO_REV(0x02AF), NATIVE_AUDIO_REV(0x0372), NATIVE_AUDIO_REV(0x0266),
                                     NATIVE_AUDIO_REV(0x031C), NATIVE_AUDIO_REV(0x025D), NATIVE_AUDIO_REV(0x025C), NATIVE_AUDIO_REV(0x018E),
                                     NATIVE_AUDIO_REV(0x022F), NATIVE_AUDIO_REV(0x0135), NATIVE_AUDIO_REV(0x01D2), NATIVE_AUDIO_REV(0x00B7),
                                     NATIVE_AUDIO_REV(0x018F), NATIVE_AUDIO_REV(0x00B5), NATIVE_AUDIO_REV(0x00B4), NATIVE_AUDIO_REV(0x0080),
                                     NATIVE_AUDIO_REV(0x004C), NATIVE_AUDIO_REV(0x0026), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_STUDIO_B, 0x4840, {NATIVE_AUDIO_REV(0x00B1), NATIVE_AUDIO_REV(0x007F), NATIVE_AUDIO_REV(0x70F0), NATIVE_AUDIO_REV(0x4FA8),
                                     NATIVE_AUDIO_REV(0xBCE0), NATIVE_AUDIO_REV(0x4510), NATIVE_AUDIO_REV(0xBEF0), NATIVE_AUDIO_REV(0xB4C0),
                                     NATIVE_AUDIO_REV(0x5280), NATIVE_AUDIO_REV(0x4EC0), NATIVE_AUDIO_REV(0x0904), NATIVE_AUDIO_REV(0x076B),
                                     NATIVE_AUDIO_REV(0x0824), NATIVE_AUDIO_REV(0x065F), NATIVE_AUDIO_REV(0x07A2), NATIVE_AUDIO_REV(0x0616),
                                     NATIVE_AUDIO_REV(0x076C), NATIVE_AUDIO_REV(0x05ED), NATIVE_AUDIO_REV(0x05EC), NATIVE_AUDIO_REV(0x042E),
                                     NATIVE_AUDIO_REV(0x050F), NATIVE_AUDIO_REV(0x0305), NATIVE_AUDIO_REV(0x0462), NATIVE_AUDIO_REV(0x02B7),
                                     NATIVE_AUDIO_REV(0x042F), NATIVE_AUDIO_REV(0x0265), NATIVE_AUDIO_REV(0x0264), NATIVE_AUDIO_REV(0x01B2),
                                     NATIVE_AUDIO_REV(0x0100), NATIVE_AUDIO_REV(0x0080), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_STUDIO_C, 0x6FE0, {NATIVE_AUDIO_REV(0x00E3), NATIVE_AUDIO_REV(0x00A9), NATIVE_AUDIO_REV(0x6F60), NATIVE_AUDIO_REV(0x4FA8),
                                     NATIVE_AUDIO_REV(0xBCE0), NATIVE_AUDIO_REV(0x4510), NATIVE_AUDIO_REV(0xBEF0), NATIVE_AUDIO_REV(0xA680),
                                     NATIVE_AUDIO_REV(0x5680), NATIVE_AUDIO_REV(0x52C0), NATIVE_AUDIO_REV(0x0DFB), NATIVE_AUDIO_REV(0x0B58),
                                     NATIVE_AUDIO_REV(0x0D09), NATIVE_AUDIO_REV(0x0A3C), NATIVE_AUDIO_REV(0x0BD9), NATIVE_AUDIO_REV(0x0973),
                                     NATIVE_AUDIO_REV(0x0B59), NATIVE_AUDIO_REV(0x08DA), NATIVE_AUDIO_REV(0x08D9), NATIVE_AUDIO_REV(0x05E9),
                                     NATIVE_AUDIO_REV(0x07EC), NATIVE_AUDIO_REV(0x04B0), NATIVE_AUDIO_REV(0x06EF), NATIVE_AUDIO_REV(0x03D2),
                                     NATIVE_AUDIO_REV(0x05EA), NATIVE_AUDIO_REV(0x031D), NATIVE_AUDIO_REV(0x031C), NATIVE_AUDIO_REV(0x0238),
                                     NATIVE_AUDIO_REV(0x0154), NATIVE_AUDIO_REV(0x00AA), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_HALL, 0xADE0, {NATIVE_AUDIO_REV(0x01A5), NATIVE_AUDIO_REV(0x0139), NATIVE_AUDIO_REV(0x6000), NATIVE_AUDIO_REV(0x5000),
                                 NATIVE_AUDIO_REV(0x4C00), NATIVE_AUDIO_REV(0xB800), NATIVE_AUDIO_REV(0xBC00), NATIVE_AUDIO_REV(0xC000),
                                 NATIVE_AUDIO_REV(0x6000), NATIVE_AUDIO_REV(0x5C00), NATIVE_AUDIO_REV(0x15BA), NATIVE_AUDIO_REV(0x11BB),
                                 NATIVE_AUDIO_REV(0x14C2), NATIVE_AUDIO_REV(0x10BD), NATIVE_AUDIO_REV(0x11BC), NATIVE_AUDIO_REV(0x0DC1),
                                 NATIVE_AUDIO_REV(0x11C0), NATIVE_AUDIO_REV(0x0DC3), NATIVE_AUDIO_REV(0x0DC0), NATIVE_AUDIO_REV(0x09C1),
                                 NATIVE_AUDIO_REV(0x0BC4), NATIVE_AUDIO_REV(0x07C1), NATIVE_AUDIO_REV(0x0A00), NATIVE_AUDIO_REV(0x06CD),
                                 NATIVE_AUDIO_REV(0x09C2), NATIVE_AUDIO_REV(0x05C1), NATIVE_AUDIO_REV(0x05C0), NATIVE_AUDIO_REV(0x041A),
                                 NATIVE_AUDIO_REV(0x0274), NATIVE_AUDIO_REV(0x013A), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_SPACE, 0xF6C0, {NATIVE_AUDIO_REV(0x033D), NATIVE_AUDIO_REV(0x0231), NATIVE_AUDIO_REV(0x7E00), NATIVE_AUDIO_REV(0x5000),
                                  NATIVE_AUDIO_REV(0xB400), NATIVE_AUDIO_REV(0xB000), NATIVE_AUDIO_REV(0x4C00), NATIVE_AUDIO_REV(0xB000),
                                  NATIVE_AUDIO_REV(0x6000), NATIVE_AUDIO_REV(0x5400), NATIVE_AUDIO_REV(0x1ED6), NATIVE_AUDIO_REV(0x1A31),
                                  NATIVE_AUDIO_REV(0x1D14), NATIVE_AUDIO_REV(0x183B), NATIVE_AUDIO_REV(0x1BC2), NATIVE_AUDIO_REV(0x16B2),
                                  NATIVE_AUDIO_REV(0x1A32), NATIVE_AUDIO_REV(0x15EF), NATIVE_AUDIO_REV(0x15EE), NATIVE_AUDIO_REV(0x1055),
                                  NATIVE_AUDIO_REV(0x1334), NATIVE_AUDIO_REV(0x0F2D), NATIVE_AUDIO_REV(0x11F6), NATIVE_AUDIO_REV(0x0C5D),
                                  NATIVE_AUDIO_REV(0x1056), NATIVE_AUDIO_REV(0x0AE1), NATIVE_AUDIO_REV(0x0AE0), NATIVE_AUDIO_REV(0x07A2),
                                  NATIVE_AUDIO_REV(0x0464), NATIVE_AUDIO_REV(0x0232), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_ECHO, 0x18040, {NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x7FFF), NATIVE_AUDIO_REV(0x7FFF),
                                  NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x8100),
                                  NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x1FFF), NATIVE_AUDIO_REV(0x0FFF),
                                  NATIVE_AUDIO_REV(0x1005), NATIVE_AUDIO_REV(0x0005), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                  NATIVE_AUDIO_REV(0x1005), NATIVE_AUDIO_REV(0x0005), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                  NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                  NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x1004), NATIVE_AUDIO_REV(0x1002),
                                  NATIVE_AUDIO_REV(0x0004), NATIVE_AUDIO_REV(0x0002), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_DELAY, 0x18040, {NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x0001), NATIVE_AUDIO_REV(0x7FFF), NATIVE_AUDIO_REV(0x7FFF),
                                   NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                   NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x1FFF), NATIVE_AUDIO_REV(0x0FFF),
                                   NATIVE_AUDIO_REV(0x1005), NATIVE_AUDIO_REV(0x0005), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                   NATIVE_AUDIO_REV(0x1005), NATIVE_AUDIO_REV(0x0005), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                   NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000),
                                   NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x0000), NATIVE_AUDIO_REV(0x1004), NATIVE_AUDIO_REV(0x1002),
                                   NATIVE_AUDIO_REV(0x0004), NATIVE_AUDIO_REV(0x0002), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
    {SPU_REV_MODE_PIPE, 0x3C00, {NATIVE_AUDIO_REV(0x0017), NATIVE_AUDIO_REV(0x0013), NATIVE_AUDIO_REV(0x70F0), NATIVE_AUDIO_REV(0x4FA8),
                                 NATIVE_AUDIO_REV(0xBCE0), NATIVE_AUDIO_REV(0x4510), NATIVE_AUDIO_REV(0xBEF0), NATIVE_AUDIO_REV(0x8500),
                                 NATIVE_AUDIO_REV(0x5F80), NATIVE_AUDIO_REV(0x54C0), NATIVE_AUDIO_REV(0x0371), NATIVE_AUDIO_REV(0x02AF),
                                 NATIVE_AUDIO_REV(0x02E5), NATIVE_AUDIO_REV(0x01DF), NATIVE_AUDIO_REV(0x02B0), NATIVE_AUDIO_REV(0x01D7),
                                 NATIVE_AUDIO_REV(0x0358), NATIVE_AUDIO_REV(0x026A), NATIVE_AUDIO_REV(0x01D6), NATIVE_AUDIO_REV(0x011E),
                                 NATIVE_AUDIO_REV(0x012D), NATIVE_AUDIO_REV(0x00B1), NATIVE_AUDIO_REV(0x011F), NATIVE_AUDIO_REV(0x0059),
                                 NATIVE_AUDIO_REV(0x01A0), NATIVE_AUDIO_REV(0x00E3), NATIVE_AUDIO_REV(0x0058), NATIVE_AUDIO_REV(0x0040),
                                 NATIVE_AUDIO_REV(0x0028), NATIVE_AUDIO_REV(0x0014), NATIVE_AUDIO_REV(0x8000), NATIVE_AUDIO_REV(0x8000)}},
};

#undef NATIVE_AUDIO_REV

internal int NativeAudio_Clamp16(int value)
{
	if (value > 32767)
	{
		return 32767;
	}
	if (value < -32768)
	{
		return -32768;
	}
	return value;
}

internal int NativeAudio_VolumeScale(int volume)
{
	if (volume < 0)
	{
		return 0;
	}
	if (volume > NATIVE_AUDIO_DIRECT_VOL_MAX)
	{
		return NATIVE_AUDIO_DIRECT_VOL_MAX;
	}
	return volume;
}

internal int NativeAudio_ApplyVolume(int sample, s16 volume, s16 masterVolume)
{
	int scaledVolume = NativeAudio_VolumeScale(volume);
	int scaledMasterVolume = NativeAudio_VolumeScale(masterVolume);
	return (int)(((s64)sample * scaledVolume * scaledMasterVolume) / (NATIVE_AUDIO_DIRECT_VOL_MAX * NATIVE_AUDIO_DIRECT_VOL_MAX));
}

internal int NativeAudio_ApplyMasterVolume(int sample, s16 masterVolume)
{
	int scaledMasterVolume = NativeAudio_VolumeScale(masterVolume);

	return (int)(((s64)sample * scaledMasterVolume) / NATIVE_AUDIO_DIRECT_VOL_MAX);
}

internal const struct NativeAudioReverbPreset *NativeAudio_FindReverbPreset(int mode)
{
	int i;

	for (i = 0; i < (int)(sizeof(s_reverbPresets) / sizeof(s_reverbPresets[0])); i++)
	{
		if (s_reverbPresets[i].mode == mode)
		{
			return &s_reverbPresets[i];
		}
	}

	return &s_reverbPresets[0];
}

internal int NativeAudio_ReverbModeFromRaw(int mode)
{
	mode &= 0xff;
	if ((mode < SPU_REV_MODE_OFF) || (mode >= SPU_REV_MODE_MAX))
	{
		mode = SPU_REV_MODE_OFF;
	}
	return mode;
}

internal int NativeAudio_ReverbOffsetSamples(s16 reg)
{
	return (int)((u16)reg) * 4;
}

internal int NativeAudio_ReverbWrapIndex(int index, int sizeSamples)
{
	if (sizeSamples <= 0)
	{
		return 0;
	}

	index %= sizeSamples;
	if (index < 0)
	{
		index += sizeSamples;
	}
	return index;
}

internal int NativeAudio_ReverbRead(const struct NativeAudioReverbPreset *preset, int reg, int deltaSamples)
{
	int index;

	if (s_audio.reverb.sizeSamples <= 0)
	{
		return 0;
	}

	index = s_audio.reverb.cursor + NativeAudio_ReverbOffsetSamples(preset->reg[reg]) + deltaSamples;
	index = NativeAudio_ReverbWrapIndex(index, s_audio.reverb.sizeSamples);
	return s_audio.reverb.buffer[index];
}

internal int NativeAudio_ReverbReadAtOffset(int offsetSamples)
{
	int index;

	if (s_audio.reverb.sizeSamples <= 0)
	{
		return 0;
	}

	index = NativeAudio_ReverbWrapIndex(s_audio.reverb.cursor + offsetSamples, s_audio.reverb.sizeSamples);
	return s_audio.reverb.buffer[index];
}

internal void NativeAudio_ReverbWrite(const struct NativeAudioReverbPreset *preset, int reg, int value)
{
	int index;

	if (s_audio.reverb.sizeSamples <= 0)
	{
		return;
	}

	index = s_audio.reverb.cursor + NativeAudio_ReverbOffsetSamples(preset->reg[reg]);
	index = NativeAudio_ReverbWrapIndex(index, s_audio.reverb.sizeSamples);
	s_audio.reverb.buffer[index] = (s16)NativeAudio_Clamp16(value);
}

internal int NativeAudio_ReverbMul(int sample, s16 volume)
{
	return (int)(((s64)sample * (int)volume) / 0x8000);
}

internal int NativeAudio_ReverbFirApply(const s16 *history, s32 cursor)
{
	s64 sum = 0;
	int i;

	for (i = 0; i < NATIVE_AUDIO_REVERB_FIR_TAPS; i++)
	{
		int index = (cursor + i) % NATIVE_AUDIO_REVERB_FIR_TAPS;

		sum += (s64)s_reverbFirCoeffs[i] * history[index];
	}

	return NativeAudio_Clamp16((int)(sum / 0x8000));
}

internal int NativeAudio_ReverbFirApplyUpsampled(const s16 *history, s32 cursor)
{
	return NativeAudio_Clamp16(NativeAudio_ReverbFirApply(history, cursor) * 2);
}

internal void NativeAudio_ReverbPushInputSampleNoLock(int left, int right)
{
	s_audio.reverb.inputHistoryLeft[s_audio.reverb.inputHistoryCursor] = (s16)NativeAudio_Clamp16(left);
	s_audio.reverb.inputHistoryRight[s_audio.reverb.inputHistoryCursor] = (s16)NativeAudio_Clamp16(right);
	s_audio.reverb.inputHistoryCursor = (s_audio.reverb.inputHistoryCursor + 1) % NATIVE_AUDIO_REVERB_FIR_TAPS;
}

internal void NativeAudio_ReverbPushOutputSampleNoLock(int left, int right)
{
	s_audio.reverb.outputHistoryLeft[s_audio.reverb.outputHistoryCursor] = (s16)NativeAudio_Clamp16(left);
	s_audio.reverb.outputHistoryRight[s_audio.reverb.outputHistoryCursor] = (s16)NativeAudio_Clamp16(right);
	s_audio.reverb.outputHistoryCursor = (s_audio.reverb.outputHistoryCursor + 1) % NATIVE_AUDIO_REVERB_FIR_TAPS;
}

internal void NativeAudio_ReverbClearBufferNoLock(void)
{
	memset(s_audio.reverb.buffer, 0, sizeof(s_audio.reverb.buffer));
	memset(s_audio.reverb.inputHistoryLeft, 0, sizeof(s_audio.reverb.inputHistoryLeft));
	memset(s_audio.reverb.inputHistoryRight, 0, sizeof(s_audio.reverb.inputHistoryRight));
	memset(s_audio.reverb.outputHistoryLeft, 0, sizeof(s_audio.reverb.outputHistoryLeft));
	memset(s_audio.reverb.outputHistoryRight, 0, sizeof(s_audio.reverb.outputHistoryRight));
	s_audio.reverb.cursor = 0;
	s_audio.reverb.inputHistoryCursor = 0;
	s_audio.reverb.outputHistoryCursor = 0;
	s_audio.reverb.samplePhase = 0;
	s_audio.reverb.lastOutLeft = 0;
	s_audio.reverb.lastOutRight = 0;
}

internal void NativeAudio_ReverbConfigureModeNoLock(int rawMode)
{
	const struct NativeAudioReverbPreset *preset;
	int mode = NativeAudio_ReverbModeFromRaw(rawMode);
	int clearWorkArea = (rawMode & SPU_REV_MODE_CLEAR_WA) != 0;
	int oldMode = s_audio.reverb.mode;
	int oldSizeSamples = s_audio.reverb.sizeSamples;

	preset = NativeAudio_FindReverbPreset(mode);
	s_audio.reverb.mode = preset->mode;
	s_audio.reverb.sizeSamples = preset->sizeBytes / (int)sizeof(s16);

	if ((s_audio.reverb.sizeSamples < 0) || (s_audio.reverb.sizeSamples > NATIVE_AUDIO_REVERB_MAX_SAMPLES))
	{
		s_audio.reverb.sizeSamples = 0;
	}

	if (clearWorkArea || (oldMode != s_audio.reverb.mode) || (oldSizeSamples != s_audio.reverb.sizeSamples))
	{
		NativeAudio_ReverbClearBufferNoLock();
	}
}

internal int NativeAudio_ReverbRunReflectionStage(const struct NativeAudioReverbPreset *preset, int input, int feedbackReg, int writeReg)
{
	int previous = NativeAudio_ReverbRead(preset, writeReg, -1);
	int feedback = NativeAudio_ReverbMul(NativeAudio_ReverbRead(preset, feedbackReg, 0), preset->reg[NATIVE_AUDIO_REV_VWALL]);
	int value = NativeAudio_ReverbMul(input + feedback - previous, preset->reg[NATIVE_AUDIO_REV_VIIR]) + previous;

	return value;
}

internal int NativeAudio_ReverbRunCombStage(const struct NativeAudioReverbPreset *preset, int baseReg)
{
	int value = 0;

	value += NativeAudio_ReverbMul(NativeAudio_ReverbRead(preset, baseReg, 0), preset->reg[NATIVE_AUDIO_REV_VCOMB1]);
	value += NativeAudio_ReverbMul(NativeAudio_ReverbRead(preset, baseReg + 2, 0), preset->reg[NATIVE_AUDIO_REV_VCOMB2]);
	value += NativeAudio_ReverbMul(NativeAudio_ReverbRead(preset, baseReg + 8, 0), preset->reg[NATIVE_AUDIO_REV_VCOMB3]);
	value += NativeAudio_ReverbMul(NativeAudio_ReverbRead(preset, baseReg + 10, 0), preset->reg[NATIVE_AUDIO_REV_VCOMB4]);
	return value;
}

internal int NativeAudio_ReverbRunApfStage(const struct NativeAudioReverbPreset *preset, int input, int apfReg, int deltaReg, int volumeReg)
{
	int delta = NativeAudio_ReverbOffsetSamples(preset->reg[apfReg]) - NativeAudio_ReverbOffsetSamples(preset->reg[deltaReg]);
	int delayed = NativeAudio_ReverbReadAtOffset(delta);
	int stored = input - NativeAudio_ReverbMul(delayed, preset->reg[volumeReg]);

	if (s_audio.reverbEnabled)
	{
		NativeAudio_ReverbWrite(preset, apfReg, stored);
	}
	delayed = NativeAudio_ReverbReadAtOffset(delta);
	return NativeAudio_ReverbMul(stored, preset->reg[volumeReg]) + delayed;
}

internal void NativeAudio_ReverbProcessNoLock(int sendLeft, int sendRight, int *wetLeft, int *wetRight)
{
	const struct NativeAudioReverbPreset *preset;
	int processThisFrame;
	int lin;
	int rin;
	int sameLeft;
	int sameRight;
	int diffLeft;
	int diffRight;
	int outLeft;
	int outRight;

	NativeAudio_ReverbPushInputSampleNoLock(sendLeft, sendRight);

	s_audio.reverb.samplePhase ^= 1;
	processThisFrame = s_audio.reverb.samplePhase == 0;

	if (!processThisFrame || s_audio.reverb.sizeSamples <= 0)
	{
		NativeAudio_ReverbPushOutputSampleNoLock(0, 0);
		goto output_fir;
	}

	preset = NativeAudio_FindReverbPreset(s_audio.reverb.mode);
	lin = NativeAudio_ReverbMul(NativeAudio_ReverbFirApply(s_audio.reverb.inputHistoryLeft, s_audio.reverb.inputHistoryCursor),
	                            preset->reg[NATIVE_AUDIO_REV_VLIN]);
	rin = NativeAudio_ReverbMul(NativeAudio_ReverbFirApply(s_audio.reverb.inputHistoryRight, s_audio.reverb.inputHistoryCursor),
	                            preset->reg[NATIVE_AUDIO_REV_VRIN]);

	sameLeft = NativeAudio_ReverbRunReflectionStage(preset, lin, NATIVE_AUDIO_REV_DLSAME, NATIVE_AUDIO_REV_MLSAME);
	sameRight = NativeAudio_ReverbRunReflectionStage(preset, rin, NATIVE_AUDIO_REV_DRSAME, NATIVE_AUDIO_REV_MRSAME);
	diffLeft = NativeAudio_ReverbRunReflectionStage(preset, lin, NATIVE_AUDIO_REV_DRDIFF, NATIVE_AUDIO_REV_MLDIFF);
	diffRight = NativeAudio_ReverbRunReflectionStage(preset, rin, NATIVE_AUDIO_REV_DLDIFF, NATIVE_AUDIO_REV_MRDIFF);

	if (s_audio.reverbEnabled)
	{
		NativeAudio_ReverbWrite(preset, NATIVE_AUDIO_REV_MLSAME, sameLeft);
		NativeAudio_ReverbWrite(preset, NATIVE_AUDIO_REV_MRSAME, sameRight);
		NativeAudio_ReverbWrite(preset, NATIVE_AUDIO_REV_MLDIFF, diffLeft);
		NativeAudio_ReverbWrite(preset, NATIVE_AUDIO_REV_MRDIFF, diffRight);
	}

	outLeft = NativeAudio_ReverbRunCombStage(preset, NATIVE_AUDIO_REV_MLCOMB1);
	outRight = NativeAudio_ReverbRunCombStage(preset, NATIVE_AUDIO_REV_MRCOMB1);
	outLeft = NativeAudio_ReverbRunApfStage(preset, outLeft, NATIVE_AUDIO_REV_MLAPF1, NATIVE_AUDIO_REV_DAPF1, NATIVE_AUDIO_REV_VAPF1);
	outRight = NativeAudio_ReverbRunApfStage(preset, outRight, NATIVE_AUDIO_REV_MRAPF1, NATIVE_AUDIO_REV_DAPF1, NATIVE_AUDIO_REV_VAPF1);
	outLeft = NativeAudio_ReverbRunApfStage(preset, outLeft, NATIVE_AUDIO_REV_MLAPF2, NATIVE_AUDIO_REV_DAPF2, NATIVE_AUDIO_REV_VAPF2);
	outRight = NativeAudio_ReverbRunApfStage(preset, outRight, NATIVE_AUDIO_REV_MRAPF2, NATIVE_AUDIO_REV_DAPF2, NATIVE_AUDIO_REV_VAPF2);
	NativeAudio_ReverbPushOutputSampleNoLock(outLeft, outRight);
	s_audio.reverb.cursor = NativeAudio_ReverbWrapIndex(s_audio.reverb.cursor + 1, s_audio.reverb.sizeSamples);

output_fir:
	outLeft = NativeAudio_ReverbFirApplyUpsampled(s_audio.reverb.outputHistoryLeft, s_audio.reverb.outputHistoryCursor);
	outRight = NativeAudio_ReverbFirApplyUpsampled(s_audio.reverb.outputHistoryRight, s_audio.reverb.outputHistoryCursor);
	outLeft = NativeAudio_ReverbMul(outLeft, s_audio.reverbAttr.depth.left);
	outRight = NativeAudio_ReverbMul(outRight, s_audio.reverbAttr.depth.right);
	s_audio.reverb.lastOutLeft = (s16)NativeAudio_Clamp16(outLeft);
	s_audio.reverb.lastOutRight = (s16)NativeAudio_Clamp16(outRight);
	*wetLeft = s_audio.reverb.lastOutLeft;
	*wetRight = s_audio.reverb.lastOutRight;
}

internal s16 NativeAudio_GetVoicePcmSample(const struct NativeAudioVoice *voice, int sampleIndex)
{
	if (voice->looped && voice->loopEnabled && (voice->loopEnd > voice->loopStart))
	{
		int loopLen = voice->loopEnd - voice->loopStart;

		while (sampleIndex < voice->loopStart)
		{
			sampleIndex += loopLen;
		}
		while (sampleIndex >= voice->loopEnd)
		{
			sampleIndex -= loopLen;
		}
	}

	if ((sampleIndex < 0) || (sampleIndex >= voice->sampleCount))
	{
		return 0;
	}

	return voice->pcm[sampleIndex];
}

internal int NativeAudio_InterpolateVoiceSample(const struct NativeAudioVoice *voice)
{
	int sampleIndex = (int)(voice->positionFp >> NATIVE_AUDIO_FP_SHIFT);
	int gaussIndex = (int)((voice->positionFp >> NATIVE_AUDIO_GAUSS_INDEX_SHIFT) & 0xff);
	int oldest = NativeAudio_GetVoicePcmSample(voice, sampleIndex - 3);
	int older = NativeAudio_GetVoicePcmSample(voice, sampleIndex - 2);
	int old = NativeAudio_GetVoicePcmSample(voice, sampleIndex - 1);
	int newest = NativeAudio_GetVoicePcmSample(voice, sampleIndex);
	int sample;

	sample = (s_gaussTable[0xff - gaussIndex] * oldest) >> 15;
	sample += (s_gaussTable[0x1ff - gaussIndex] * older) >> 15;
	sample += (s_gaussTable[0x100 + gaussIndex] * old) >> 15;
	sample += (s_gaussTable[gaussIndex] * newest) >> 15;

	return NativeAudio_Clamp16(sample);
}

internal u64 NativeAudio_GetXAOutputFrameCount(int frameCount, int sampleRate)
{
	if ((frameCount <= 0) || (sampleRate <= 0))
	{
		return 0;
	}

	if (sampleRate == XA_SAMPLE_RATE_37800)
	{
		return (((u64)frameCount * NATIVE_AUDIO_XA_ZIGZAG_PHASES) + (NATIVE_AUDIO_XA_ZIGZAG_INPUTS - 1)) / NATIVE_AUDIO_XA_ZIGZAG_INPUTS;
	}
	if (sampleRate == XA_SAMPLE_RATE_18900)
	{
		return (((u64)frameCount * NATIVE_AUDIO_XA_ZIGZAG_PHASES) + ((NATIVE_AUDIO_XA_ZIGZAG_INPUTS / 2) - 1)) / (NATIVE_AUDIO_XA_ZIGZAG_INPUTS / 2);
	}

	return (((u64)frameCount * NATIVE_AUDIO_SAMPLE_RATE) + ((u64)sampleRate - 1)) / (u64)sampleRate;
}

internal void NativeAudio_UpdateXAPositionFromOutputFrameNoLock(void)
{
	if (s_audio.xa.sampleRate <= 0)
	{
		s_audio.xa.positionFp = 0;
		return;
	}

	s_audio.xa.positionFp = ((s_audio.xa.outputFrame * (u64)s_audio.xa.sampleRate) << NATIVE_AUDIO_FP_SHIFT) / NATIVE_AUDIO_SAMPLE_RATE;
}

internal void NativeAudio_AdvanceXAOutputFrameNoLock(void)
{
	u64 outputFrameCount;

	s_audio.xa.outputFrame++;
	outputFrameCount = NativeAudio_GetXAOutputFrameCount(s_audio.xa.frameCount, s_audio.xa.sampleRate);
	if ((outputFrameCount > 0) && (s_audio.xa.outputFrame >= outputFrameCount))
	{
		s_audio.xa.outputFrame = outputFrameCount;
		s_audio.xa.active = 0;
	}
	NativeAudio_UpdateXAPositionFromOutputFrameNoLock();
}

internal int NativeAudio_GetXAPcmSampleAtFrameNoLock(int channel, u64 frameIndex)
{
	return NativeAudio_XARingReadSampleNoLock(&s_audio.xaStream, channel, frameIndex);
}

internal int NativeAudio_GetXAPseudo37800SampleNoLock(int channel, s64 pseudoFrameIndex)
{
	u64 frameIndex;

	if (pseudoFrameIndex < 0)
	{
		return 0;
	}

	frameIndex = (u64)pseudoFrameIndex;
	// NOTE(aalhendi): 18.9kHz XA feeds the same 37.8kHz->44.1kHz zig-zag path with each decoded source frame held twice.
	if (s_audio.xa.sampleRate == XA_SAMPLE_RATE_18900)
	{
		frameIndex >>= 1;
	}

	return NativeAudio_GetXAPcmSampleAtFrameNoLock(channel, frameIndex);
}

internal int NativeAudio_ZigZagInterpolateXASampleNoLock(int channel)
{
	u64 group = s_audio.xa.outputFrame / NATIVE_AUDIO_XA_ZIGZAG_PHASES;
	int phase = (int)(s_audio.xa.outputFrame % NATIVE_AUDIO_XA_ZIGZAG_PHASES);
	u64 p = (group + 1) * NATIVE_AUDIO_XA_ZIGZAG_INPUTS;
	int sum = 0;
	int tap;

	for (tap = 0; tap < NATIVE_AUDIO_XA_ZIGZAG_TAPS; tap++)
	{
		int sample = NativeAudio_GetXAPseudo37800SampleNoLock(channel, (s64)p - (s64)(tap + 1));
		sum += (int)(((s64)sample * s_xaZigZagTable[tap][phase]) >> 15);
	}

	return NativeAudio_Clamp16(sum);
}

internal int NativeAudio_InterpolateXALinearSampleNoLock(int channel)
{
	u64 frameIndex = s_audio.xa.positionFp >> NATIVE_AUDIO_FP_SHIFT;
	u32 frac = (u32)(s_audio.xa.positionFp & (NATIVE_AUDIO_FP_ONE - 1));
	int a = NativeAudio_GetXAPcmSampleAtFrameNoLock(channel, frameIndex);
	int b = a;

	if (frameIndex + 1 < (u64)s_audio.xa.frameCount)
	{
		b = NativeAudio_GetXAPcmSampleAtFrameNoLock(channel, frameIndex + 1);
	}

	return a + (int)(((s64)(b - a) * frac) >> NATIVE_AUDIO_FP_SHIFT);
}

internal int NativeAudio_GetXAMixSampleNoLock(int channel)
{
	if ((s_audio.xa.sampleRate == XA_SAMPLE_RATE_37800) || (s_audio.xa.sampleRate == XA_SAMPLE_RATE_18900))
	{
		return NativeAudio_ZigZagInterpolateXASampleNoLock(channel);
	}

	return NativeAudio_InterpolateXALinearSampleNoLock(channel);
}

// NOTE(aalhendi): PS1 SPU ADSR envelope behavior follows PSX-SPX.
// Source reference: https://psx-spx.consoledev.net/soundprocessingunitspu/
internal int NativeAudio_ClampAdsrLevel(int level)
{
	if (level < NATIVE_AUDIO_ADSR_MIN)
	{
		return NATIVE_AUDIO_ADSR_MIN;
	}
	if (level > NATIVE_AUDIO_ADSR_MAX)
	{
		return NATIVE_AUDIO_ADSR_MAX;
	}
	return level;
}

internal b32 NativeAudio_AdsrModeIsExponential(int mode)
{
	return mode == SPU_VOICE_EXPIncN || mode == SPU_VOICE_EXPIncR || mode == SPU_VOICE_EXPDec;
}

internal b32 NativeAudio_AdsrModeIsDecreasing(int mode)
{
	return mode == SPU_VOICE_LINEARDecN || mode == SPU_VOICE_LINEARDecR || mode == SPU_VOICE_EXPDec;
}

internal b32 NativeAudio_AdsrModeIsPhaseNegative(int mode)
{
	return mode == SPU_VOICE_LINEARIncR || mode == SPU_VOICE_LINEARDecR || mode == SPU_VOICE_EXPIncR;
}

internal int NativeAudio_AdsrSustainTarget(const struct NativeAudioVoice *voice)
{
	int target = ((int)voice->attr.sl + 1) * 0x800;

	if (target > NATIVE_AUDIO_ADSR_MAX)
	{
		target = NATIVE_AUDIO_ADSR_MAX;
	}
	return target;
}

internal void NativeAudio_AdsrSetPhase(struct NativeAudioVoice *voice, int phase)
{
	voice->adsrPhase = (u8)phase;
	voice->adsrCounter = 0;
}

internal void NativeAudio_AdsrForceOff(struct NativeAudioVoice *voice)
{
	voice->adsrLevel = 0;
	voice->adsrCounter = 0;
	voice->adsrPhase = NATIVE_AUDIO_ADSR_OFF;
	voice->attr.envx = 0;
	voice->active = 0;
}

internal void NativeAudio_AdsrKeyOn(struct NativeAudioVoice *voice)
{
	voice->adsrLevel = 0;
	voice->adsrCounter = 0;
	voice->adsrPhase = NATIVE_AUDIO_ADSR_ATTACK;
	voice->attr.envx = 0;
}

internal void NativeAudio_AdsrKeyOff(struct NativeAudioVoice *voice)
{
	if (voice->adsrPhase != NATIVE_AUDIO_ADSR_OFF)
	{
		NativeAudio_AdsrSetPhase(voice, NATIVE_AUDIO_ADSR_RELEASE);
	}
	else
	{
		NativeAudio_AdsrForceOff(voice);
	}
}

internal b32 NativeAudio_AdsrRateIsAllOnes(int shiftValue, int stepValue, int bitCount)
{
	int mask = (1 << bitCount) - 1;

	return (stepValue | (shiftValue << 2)) == mask;
}

/*
 * The two quantities an envelope step is made of: the delta applied to
 * adsrLevel when the counter overflows, and the per-sample counter
 * increment. Split out of NativeAudio_AdsrRunEnvelopeStep (which was their
 * only consumer) so the 3DS lifecycle fast path can ask "how long until this
 * counter overflows?" from the SAME arithmetic rather than a copy of it --
 * a copy would silently drift the moment either is touched. Pure extraction:
 * the body below is the original code verbatim, and the golden mixer test
 * is what proves it.
 */
struct NativeAudioAdsrStep
{
	int level;     /* signed delta applied to adsrLevel on counter overflow */
	u32 increment; /* counter increment per sample; never 0 */
};

internal void NativeAudio_AdsrDeriveStep(const struct NativeAudioVoice *voice, int shiftValue, int stepValue, b32 exponential, b32 decreasing,
                                         b32 phaseNegative, struct NativeAudioAdsrStep *out)
{
	int adsrStep;
	u32 counterIncrement;

	adsrStep = 7 - stepValue;
	if (decreasing != phaseNegative)
	{
		adsrStep = ~adsrStep;
	}

	if (shiftValue < 11)
	{
		adsrStep <<= 11 - shiftValue;
	}

	counterIncrement = NATIVE_AUDIO_ADSR_STEP_BIT;
	if (shiftValue > 11)
	{
		int shift = shiftValue - 11;
		counterIncrement = shift < 31 ? (NATIVE_AUDIO_ADSR_STEP_BIT >> shift) : 0;
	}

	if (exponential && !decreasing && voice->adsrLevel > 0x6000)
	{
		if (shiftValue < 10)
		{
			adsrStep >>= 2;
		}
		else if (shiftValue >= 11)
		{
			counterIncrement >>= 2;
		}
		else
		{
			adsrStep >>= 1;
			counterIncrement >>= 1;
		}
	}
	else if (exponential && decreasing)
	{
		adsrStep = (int)(((s64)adsrStep * voice->adsrLevel) / 0x8000);
		if (voice->adsrPhase == NATIVE_AUDIO_ADSR_RELEASE && adsrStep == 0 && voice->adsrLevel > 0)
		{
			adsrStep = -1;
		}
	}

	if (counterIncrement == 0)
	{
		counterIncrement = 1;
	}

	out->level = adsrStep;
	out->increment = counterIncrement;
}

internal void NativeAudio_AdsrRunEnvelopeStep(struct NativeAudioVoice *voice, int shiftValue, int stepValue, b32 exponential, b32 decreasing, b32 phaseNegative,
                                              b32 rateAllOnes)
{
	struct NativeAudioAdsrStep calc;
	u32 counter;

	if (rateAllOnes)
	{
		return;
	}

	NativeAudio_AdsrDeriveStep(voice, shiftValue, stepValue, exponential, decreasing, phaseNegative, &calc);

	counter = voice->adsrCounter + calc.increment;
	voice->adsrCounter = counter & (NATIVE_AUDIO_ADSR_STEP_BIT - 1);
	if ((counter & NATIVE_AUDIO_ADSR_STEP_BIT) == 0)
	{
		return;
	}

	voice->adsrLevel += calc.level;
	if (!decreasing)
	{
		voice->adsrLevel = NativeAudio_ClampAdsrLevel(voice->adsrLevel);
	}
	else if (phaseNegative)
	{
		if (voice->adsrLevel < NATIVE_AUDIO_ADSR_MIN)
		{
			voice->adsrLevel = NATIVE_AUDIO_ADSR_MIN;
		}
		if (voice->adsrLevel > 0)
		{
			voice->adsrLevel = 0;
		}
	}
	else if (voice->adsrLevel < 0)
	{
		voice->adsrLevel = 0;
	}

	voice->attr.envx = (s16)voice->adsrLevel;
}

/*
 * The (shift, step, mode-flag) tuple NativeAudio_AdsrAdvance feeds the
 * envelope step for the voice's CURRENT phase. Extracted for the same
 * single-source-of-truth reason as NativeAudio_AdsrDeriveStep above: the 3DS
 * lifecycle fast path needs the identical tuple to reason about how long the
 * phase will sit still, and a second copy of these bit twiddles would be a
 * silent-drift hazard. Returns 0 for phases with no envelope step (OFF and
 * anything unrecognized), which is the caller's cue to force the voice off.
 */
internal b32 NativeAudio_AdsrPhaseParams(const struct NativeAudioVoice *voice, int *shiftValue, int *stepValue, b32 *exponential, b32 *decreasing,
                                         b32 *phaseNegative, b32 *rateAllOnes)
{
	switch (voice->adsrPhase)
	{
	case NATIVE_AUDIO_ADSR_ATTACK:
		*shiftValue = (voice->attr.ar >> 2) & 0x1f;
		*stepValue = voice->attr.ar & 3;
		*exponential = NativeAudio_AdsrModeIsExponential(voice->attr.a_mode);
		*decreasing = 0;
		*phaseNegative = NativeAudio_AdsrModeIsPhaseNegative(voice->attr.a_mode);
		*rateAllOnes = NativeAudio_AdsrRateIsAllOnes(*shiftValue, *stepValue, 7);
		return 1;

	case NATIVE_AUDIO_ADSR_DECAY:
		*shiftValue = voice->attr.dr & 0xf;
		*stepValue = 0;
		*exponential = 1;
		*decreasing = 1;
		*phaseNegative = 0;
		*rateAllOnes = 0;
		return 1;

	case NATIVE_AUDIO_ADSR_SUSTAIN:
		*shiftValue = (voice->attr.sr >> 2) & 0x1f;
		*stepValue = voice->attr.sr & 3;
		*exponential = NativeAudio_AdsrModeIsExponential(voice->attr.s_mode);
		*decreasing = NativeAudio_AdsrModeIsDecreasing(voice->attr.s_mode);
		*phaseNegative = NativeAudio_AdsrModeIsPhaseNegative(voice->attr.s_mode);
		*rateAllOnes = NativeAudio_AdsrRateIsAllOnes(*shiftValue, *stepValue, 7);
		return 1;

	case NATIVE_AUDIO_ADSR_RELEASE:
		*shiftValue = voice->attr.rr & 0x1f;
		*stepValue = 0;
		*exponential = NativeAudio_AdsrModeIsExponential(voice->attr.r_mode);
		*decreasing = 1;
		*phaseNegative = NativeAudio_AdsrModeIsPhaseNegative(voice->attr.r_mode);
		*rateAllOnes = (*shiftValue == 0x1f);
		return 1;

	default:
		return 0;
	}
}

/*
 * How many further samples this envelope can be advanced with NO observable
 * effect beyond the counter accumulating -- i.e. how many consecutive
 * NativeAudio_AdsrAdvance calls would amount to nothing but
 * `adsrCounter += increment`. The 3DS lifecycle path uses this to jump over
 * those samples in one multiply instead of walking them.
 *
 * Deliberately pessimistic: every case this cannot PROVE is inert reports 0,
 * which sends the caller down the ordinary one-sample-at-a-time path. The
 * three guards below are the ones that matter -- each is a phase transition
 * that fires from an ENTRY test rather than from a level change, so it can
 * happen on a sample where the counter did not overflow:
 *   ATTACK  -- level already at/above max (post-test fires with no overflow)
 *   DECAY   -- level already at/below the sustain target
 *   RELEASE -- level already at/below zero
 * Everything else in the state machine moves only when the level moves, and
 * the level moves only on a counter overflow.
 *
 * `*counterIncrement` is what to add per skipped sample, or 0 when the rate
 * is all-ones (RunEnvelopeStep bails before touching the counter, so the
 * phase is frozen until the game writes new attributes -- the skip is then
 * unbounded).
 */
internal int NativeAudio_AdsrIdleSamples(const struct NativeAudioVoice *voice, int maxSamples, u32 *counterIncrement)
{
	int shiftValue;
	int stepValue;
	b32 exponential;
	b32 decreasing;
	b32 phaseNegative;
	b32 rateAllOnes;
	struct NativeAudioAdsrStep calc;
	u32 need;
	u32 steps;

	*counterIncrement = 0;

	if (maxSamples <= 0)
	{
		return 0;
	}
	if (!NativeAudio_AdsrPhaseParams(voice, &shiftValue, &stepValue, &exponential, &decreasing, &phaseNegative, &rateAllOnes))
	{
		return 0;
	}

	if ((voice->adsrPhase == NATIVE_AUDIO_ADSR_ATTACK) && (voice->adsrLevel >= NATIVE_AUDIO_ADSR_MAX))
	{
		return 0;
	}
	if ((voice->adsrPhase == NATIVE_AUDIO_ADSR_DECAY) && (voice->adsrLevel <= NativeAudio_AdsrSustainTarget(voice)))
	{
		return 0;
	}
	if ((voice->adsrPhase == NATIVE_AUDIO_ADSR_RELEASE) && (voice->adsrLevel <= 0))
	{
		return 0;
	}

	if (rateAllOnes)
	{
		return maxSamples;
	}

	NativeAudio_AdsrDeriveStep(voice, shiftValue, stepValue, exponential, decreasing, phaseNegative, &calc);

	/* adsrCounter is masked to < STEP_BIT on every write, so this cannot
	 * underflow; `steps` is the sample that overflows, so the inert run is
	 * one shorter. */
	need = NATIVE_AUDIO_ADSR_STEP_BIT - voice->adsrCounter;
	steps = (need + calc.increment - 1u) / calc.increment;
	if (steps <= 1u)
	{
		return 0;
	}

	*counterIncrement = calc.increment;
	steps--;

	return (steps > (u32)maxSamples) ? maxSamples : (int)steps;
}

internal void NativeAudio_AdsrAdvance(struct NativeAudioVoice *voice)
{
	switch (voice->adsrPhase)
	{
	case NATIVE_AUDIO_ADSR_ATTACK:
	{
		int shift = (voice->attr.ar >> 2) & 0x1f;
		int step = voice->attr.ar & 3;

		NativeAudio_AdsrRunEnvelopeStep(voice, shift, step, NativeAudio_AdsrModeIsExponential(voice->attr.a_mode), 0,
		                                NativeAudio_AdsrModeIsPhaseNegative(voice->attr.a_mode), NativeAudio_AdsrRateIsAllOnes(shift, step, 7));
		if (voice->adsrLevel >= NATIVE_AUDIO_ADSR_MAX)
		{
			voice->adsrLevel = NATIVE_AUDIO_ADSR_MAX;
			voice->attr.envx = NATIVE_AUDIO_ADSR_MAX;
			NativeAudio_AdsrSetPhase(voice, NATIVE_AUDIO_ADSR_DECAY);
		}
		break;
	}

	case NATIVE_AUDIO_ADSR_DECAY:
	{
		int target = NativeAudio_AdsrSustainTarget(voice);

		if (voice->adsrLevel <= target)
		{
			voice->adsrLevel = target;
			voice->attr.envx = (s16)target;
			NativeAudio_AdsrSetPhase(voice, NATIVE_AUDIO_ADSR_SUSTAIN);
			break;
		}

		NativeAudio_AdsrRunEnvelopeStep(voice, voice->attr.dr & 0xf, 0, 1, 1, 0, 0);
		if (voice->adsrLevel <= target)
		{
			voice->adsrLevel = target;
			voice->attr.envx = (s16)target;
			NativeAudio_AdsrSetPhase(voice, NATIVE_AUDIO_ADSR_SUSTAIN);
		}
		break;
	}

	case NATIVE_AUDIO_ADSR_SUSTAIN:
	{
		int shift = (voice->attr.sr >> 2) & 0x1f;
		int step = voice->attr.sr & 3;

		NativeAudio_AdsrRunEnvelopeStep(voice, shift, step, NativeAudio_AdsrModeIsExponential(voice->attr.s_mode),
		                                NativeAudio_AdsrModeIsDecreasing(voice->attr.s_mode), NativeAudio_AdsrModeIsPhaseNegative(voice->attr.s_mode),
		                                NativeAudio_AdsrRateIsAllOnes(shift, step, 7));
		break;
	}

	case NATIVE_AUDIO_ADSR_RELEASE:
	{
		int shift = voice->attr.rr & 0x1f;

		if (voice->adsrLevel <= 0)
		{
			NativeAudio_AdsrForceOff(voice);
			break;
		}

		NativeAudio_AdsrRunEnvelopeStep(voice, shift, 0, NativeAudio_AdsrModeIsExponential(voice->attr.r_mode), 1,
		                                NativeAudio_AdsrModeIsPhaseNegative(voice->attr.r_mode), shift == 0x1f);
		if (voice->adsrLevel <= 0)
		{
			NativeAudio_AdsrForceOff(voice);
		}
		break;
	}

	default:
		NativeAudio_AdsrForceOff(voice);
		break;
	}
}

internal int NativeAudio_ApplyAdsrEnvelope(int sample, int adsrLevel)
{
	int scaleMax;

	if (adsrLevel == 0)
	{
		return 0;
	}
	if (adsrLevel >= NATIVE_AUDIO_ADSR_MAX)
	{
		return sample;
	}

	scaleMax = adsrLevel < 0 ? 0x8000 : NATIVE_AUDIO_ADSR_MAX;
	return NativeAudio_Clamp16((int)(((s64)sample * adsrLevel) / scaleMax));
}

internal void NativeAudio_UpdatePackedAdsrFromFields(struct NativeAudioVoice *voice)
{
	voice->attr.adsr1 = (u16)((voice->attr.sl & 0xf) | ((voice->attr.dr & 0xf) << 4) | ((voice->attr.ar & 0x7f) << 8) |
	                          (NativeAudio_AdsrModeIsExponential(voice->attr.a_mode) ? 0x8000 : 0));
	voice->attr.adsr2 =
	    (u16)((voice->attr.rr & 0x1f) | ((voice->attr.sr & 0x7f) << 6) | (NativeAudio_AdsrModeIsDecreasing(voice->attr.s_mode) ? 0x4000 : 0) |
	          (NativeAudio_AdsrModeIsExponential(voice->attr.s_mode) ? 0x8000 : 0) | (NativeAudio_AdsrModeIsExponential(voice->attr.r_mode) ? 0x20 : 0));
}

internal void NativeAudio_DecodePackedAdsrToFields(struct NativeAudioVoice *voice, b32 decodeAdsr1, b32 decodeAdsr2)
{
	if (decodeAdsr1)
	{
		voice->attr.sl = voice->attr.adsr1 & 0xf;
		voice->attr.dr = (voice->attr.adsr1 >> 4) & 0xf;
		voice->attr.ar = (voice->attr.adsr1 >> 8) & 0x7f;
		voice->attr.a_mode = (voice->attr.adsr1 & 0x8000) != 0 ? SPU_VOICE_EXPIncN : SPU_VOICE_LINEARIncN;
	}

	if (decodeAdsr2)
	{
		b32 sustainDecreasing = (voice->attr.adsr2 & 0x4000) != 0;
		b32 sustainExponential = (voice->attr.adsr2 & 0x8000) != 0;

		voice->attr.rr = voice->attr.adsr2 & 0x1f;
		voice->attr.r_mode = (voice->attr.adsr2 & 0x20) != 0 ? SPU_VOICE_EXPDec : SPU_VOICE_LINEARDecN;
		voice->attr.sr = (voice->attr.adsr2 >> 6) & 0x7f;
		if (sustainExponential)
		{
			voice->attr.s_mode = sustainDecreasing ? SPU_VOICE_EXPDec : SPU_VOICE_EXPIncN;
		}
		else
		{
			voice->attr.s_mode = sustainDecreasing ? SPU_VOICE_LINEARDecN : SPU_VOICE_LINEARIncN;
		}
	}
}

internal int NativeAudio_ReadLE32(const u8 *bytes)
{
	return (int)((u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24));
}

internal int NativeAudio_ReadLE16Signed(const u8 *bytes)
{
	return (s16)((u16)bytes[0] | ((u16)bytes[1] << 8));
}

internal int NativeAudio_AlignUpInt(int value, int align)
{
	int mask = align - 1;

	if ((value < 0) || (align <= 0) || ((align & mask) != 0) || (value > INT_MAX - mask))
	{
		return -1;
	}

	return (value + mask) & ~mask;
}

internal void NativeAudio_ArenaReset(struct NativeAudioDecodeArena *arena)
{
	arena->used = 0;
}

internal void *NativeAudio_ArenaPush(struct NativeAudioDecodeArena *arena, int size, int align, int *markerOut)
{
	int marker;
	int end;

	if (size < 0)
	{
		return NULL;
	}

	marker = NativeAudio_AlignUpInt(arena->used, align);
	if ((marker < 0) || (size > INT_MAX - marker))
	{
		return NULL;
	}

	end = marker + size;
	if (end > arena->capacity)
	{
		return NULL;
	}

	if (markerOut != NULL)
	{
		*markerOut = marker;
	}
	arena->used = end;
	return &arena->memory[marker];
}

internal void NativeAudio_ArenaRewind(struct NativeAudioDecodeArena *arena, int marker)
{
	if ((marker >= 0) && (marker <= arena->used))
	{
		arena->used = marker;
	}
}

internal int NativeAudio_VoiceArenaEnsureCapacityNoLock(int capacity)
{
	struct NativeAudioDecodeArena *arena = &s_audio.voicePcmArena;
	u8 *oldMemory;
	u8 *newMemory;
	int newCapacity;
	int i;

	if (capacity < 0)
	{
		return 0;
	}
	if (capacity <= arena->capacity)
	{
		return 1;
	}

	newCapacity = arena->capacity > 0 ? arena->capacity : (256 * 1024);
	while (newCapacity < capacity)
	{
		if (newCapacity > (INT_MAX / 2))
		{
			newCapacity = capacity;
			break;
		}
		newCapacity *= 2;
	}

	oldMemory = arena->memory;
	newMemory = (u8 *)malloc((size_t)newCapacity);
	if (newMemory == NULL)
	{
		return 0;
	}

	if ((oldMemory != NULL) && (arena->used > 0))
	{
		memcpy(newMemory, oldMemory, (size_t)arena->used);
	}

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		if ((s_audio.voices[i].pcm != NULL) && (oldMemory != NULL))
		{
			u8 *pcm = (u8 *)s_audio.voices[i].pcm;
			if ((pcm >= oldMemory) && (pcm < oldMemory + arena->used))
			{
				s_audio.voices[i].pcm = (s16 *)(newMemory + (pcm - oldMemory));
			}
		}
	}

	free(oldMemory);
	arena->memory = newMemory;
	arena->capacity = newCapacity;

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
	s_voiceProf.windowArenaGrows++;
	if ((arena->capacity >> 10) > s_voiceProf.arenaPeakKiB)
	{
		s_voiceProf.arenaPeakKiB = arena->capacity >> 10;
	}
#endif

#if CTR_NATIVE_AUDIO_ARENA_LOG
	// Growth is by doubling, so this fires rarely (about once per size class) and
	// will not perturb the timing it is measuring.
	Platform_LogWarn("[audio-arena] grew to %d KiB (in use %d KiB)\n", arena->capacity >> 10, arena->used >> 10);
#endif

	return 1;
}

internal int NativeAudio_ReadFileBytes(const char *path, struct NativeAudioByteBuffer *bytes)
{
	struct NativeAssetsByteBuffer assetBytes;
	size_t pathLen;
	int readMode = NATIVE_ASSET_READ_DATA_FILE;

	bytes->data = NULL;
	bytes->size = 0;

	pathLen = strlen(path);
	if ((pathLen >= 3) && (path[pathLen - 3] == '.') && ((path[pathLen - 2] == 'X') || (path[pathLen - 2] == 'x')) &&
	    ((path[pathLen - 1] == 'A') || (path[pathLen - 1] == 'a')))
	{
		readMode = NATIVE_ASSET_READ_RAW_CD_SECTORS;
	}

	if (!NativeAssets_ReadBytes(path, readMode, &assetBytes))
	{
		return 0;
	}

	bytes->data = assetBytes.data;
	bytes->size = assetBytes.size;
	return 1;
}

internal void NativeAudio_FreeByteBuffer(struct NativeAudioByteBuffer *bytes)
{
	free(bytes->data);
	bytes->data = NULL;
	bytes->size = 0;
}

#if defined(__3DS__)
/* XA/ENG.XNF is ~1.8KB and immutable, but NativeAudio_LookupXATrackInfo used
 * to re-read it from the SD card on EVERY call -- and one voice line makes TWO
 * such calls (CDSYS_XAPlay + CDSYS_XAGetTrackLength's cooldown math), each a
 * ~26ms fixed-cost SD read on the main thread, mid-race. Cache the raw bytes
 * once; a failed first read stays failed for the session (same net behavior
 * as failing every call, without re-paying the SD probe). */
internal struct NativeAudioByteBuffer s_xnfFileCache;
internal b32 s_xnfFileCacheTried;
#endif

/* Read XA/ENG.XNF into a heap buffer the caller owns and frees -- on 3DS from
 * the one-shot RAM cache above, elsewhere straight from the asset layer. The
 * caller-owns-a-copy contract keeps every early return in the XNF parser
 * untouched. */
internal int NativeAudio_ReadXnfBytes(struct NativeAudioByteBuffer *bytes)
{
#if defined(__3DS__)
	if (!s_xnfFileCacheTried)
	{
		s_xnfFileCacheTried = 1;
		NativeAudio_ReadFileBytes("XA/ENG.XNF", &s_xnfFileCache);
	}
	if ((s_xnfFileCache.data == NULL) || (s_xnfFileCache.size <= 0))
	{
		bytes->data = NULL;
		bytes->size = 0;
		return 0;
	}
	bytes->data = malloc((size_t)s_xnfFileCache.size);
	if (bytes->data == NULL)
	{
		bytes->size = 0;
		return 0;
	}
	memcpy(bytes->data, s_xnfFileCache.data, (size_t)s_xnfFileCache.size);
	bytes->size = s_xnfFileCache.size;
	return 1;
#else
	return NativeAudio_ReadFileBytes("XA/ENG.XNF", bytes);
#endif
}

internal int NativeAudio_PcmReserve(struct NativeAudioPcmBuffer *pcm, int extra)
{
	int target;

	if ((pcm == NULL) || (extra <= 0) || (pcm->count < 0) || (pcm->capacity < 0) || (extra > INT_MAX - pcm->count))
	{
		return 0;
	}

	target = pcm->count + extra;
	return target <= pcm->capacity;
}

internal int NativeAudio_PcmPush(struct NativeAudioPcmBuffer *pcm, s16 sample)
{
	if (!NativeAudio_PcmReserve(pcm, 1))
	{
		return 0;
	}

	pcm->samples[pcm->count++] = sample;
	return 1;
}

internal int NativeAudio_DecodeAdpcmNibble(u8 soundParameter, int nibble, int *old, int *older)
{
	int shift = soundParameter & 0xf;
	int weight = (soundParameter >> 4) & 0xf;
	int sample;

	if (weight > 4)
	{
		weight = 4;
	}

	if (nibble > 7)
	{
		nibble -= 16;
	}

	sample = (nibble << 12);
	sample >>= shift;
	sample += (*old * s_posTable[weight]) >> 6;
	sample += (*older * s_negTable[weight]) >> 6;
	sample = NativeAudio_Clamp16(sample);

	*older = *old;
	*old = sample;
	return sample;
}

/*
 * Metadata-only walk of an SPU sample: same 16-byte block iteration, loop-flag
 * logic and stop condition as NativeAudio_DecodeSpuSample below, but reads
 * only each block's flags byte and counts XA_SAMPLES_PER_SOUND_UNIT per block
 * -- no nibble math, no PCM writes, roughly 1/28th the work
 *. Gives UpdateVoiceSample the
 * exact sample count to reserve (killing the old worst-case multi-MB arena
 * reservation) and owns the loop metadata so the decode below is a pure fill.
 * The two walks MUST agree on sample positions; any edit to one's loop/stop
 * logic must land in both.
 */
internal int NativeAudio_ScanSpuSampleExtent(u32 addr, u32 loopAddr, int *sampleCount, int *loopStart, int *loopEnd, b32 *loopEnabled)
{
	int count = 0;
	int block;
	int loopStartSample = 0;
	int loopEndSample = 0;
	int hasLoopStart = 0;
	int hasLoopEnd = 0;
	int hasRepeat = 0;

	if (addr >= NATIVE_AUDIO_SPU_MEMSIZE)
	{
		return 0;
	}

	for (block = (int)addr; block + 16 <= NATIVE_AUDIO_SPU_MEMSIZE; block += 16)
	{
		u8 flags = s_audio.spu.memory[block + 1];

		if ((loopAddr > addr) && (loopAddr == (u32)block))
		{
			loopStartSample = count;
			hasLoopStart = 1;
		}
		else if ((flags & ADPCM_LOOP_START) != 0)
		{
			loopStartSample = count;
			hasLoopStart = 1;
		}

		count += XA_SAMPLES_PER_SOUND_UNIT;

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
		s_voiceProf.windowScannedBlocks++;
#endif

		if ((flags & ADPCM_LOOP_END) != 0)
		{
			loopEndSample = count;
			hasLoopEnd = 1;
			hasRepeat = (flags & ADPCM_REPEAT) != 0;
			break;
		}
	}

	*sampleCount = count;
	*loopStart = hasLoopStart ? loopStartSample : 0;
	*loopEnd = hasLoopEnd ? loopEndSample : count;
	*loopEnabled = (hasLoopEnd && hasRepeat && (*loopEnd > *loopStart));
	return count > 0;
}

/*
 * Pure PCM fill: loop metadata comes from NativeAudio_ScanSpuSampleExtent
 * (which runs first and sized the buffer exactly), so this only decodes
 * nibbles and stops at the same ADPCM_LOOP_END the scan stopped at. The
 * PcmPush bounds check stays as the backstop against the two walks ever
 * disagreeing.
 */
internal int NativeAudio_DecodeSpuSample(u32 addr, struct NativeAudioPcmBuffer *pcm)
{
	int old = 0;
	int older = 0;
	int block;

	if (addr >= NATIVE_AUDIO_SPU_MEMSIZE)
	{
		return 0;
	}

	for (block = (int)addr; block + 16 <= NATIVE_AUDIO_SPU_MEMSIZE; block += 16)
	{
		u8 soundParameter = s_audio.spu.memory[block];
		u8 flags = s_audio.spu.memory[block + 1];
		int i;

		for (i = 0; i < 14; i++)
		{
			u8 packed = s_audio.spu.memory[block + 2 + i];
			if (!NativeAudio_PcmPush(pcm, (s16)NativeAudio_DecodeAdpcmNibble(soundParameter, packed & 0xf, &old, &older)))
			{
				return 0;
			}
			if (!NativeAudio_PcmPush(pcm, (s16)NativeAudio_DecodeAdpcmNibble(soundParameter, (packed >> 4) & 0xf, &old, &older)))
			{
				return 0;
			}
		}

		if ((flags & ADPCM_LOOP_END) != 0)
		{
			break;
		}
	}

	return pcm->count > 0;
}

internal void NativeAudio_FreeVoicePcm(struct NativeAudioVoice *voice)
{
	voice->pcm = NULL;
	voice->sampleCount = 0;
	voice->loopStart = 0;
	voice->loopEnd = 0;
	voice->loopEnabled = 0;
	voice->looped = 0;
}

#if defined(__3DS__)
internal struct NativeAudioHwSampleCacheEntry *NativeAudio_HwSampleCacheLookup(u32 addr)
{
	int i;

	for (i = 0; i < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; i++)
	{
		if (s_hwSampleCache[i].used && !s_hwSampleCache[i].stale && (s_hwSampleCache[i].addr == addr))
		{
			s_hwSampleCache[i].lastUse = ++s_hwSampleUseSeq;
			return &s_hwSampleCache[i];
		}
	}

	return NULL;
}

/*
 * Silence every voice still reading this slot. The DSP keeps reading the
 * block until its channel is cleared and the mirror's own stop is a vblank
 * away, so a slot that is about to be freed or overwritten has to be cut
 * here rather than left to the mirror.
 */
internal void NativeAudio_HwSampleDetachVoices(struct NativeAudioHwSampleCacheEntry *entry)
{
	int v;

	for (v = 0; v < NATIVE_AUDIO_SPU_VOICE_COUNT; v++)
	{
		if (s_audio.voices[v].pcm != entry->pcm)
		{
			continue;
		}
		N3DS_HwVoiceStop(v);
		s_hwVoiceShadow[v].started = 0;
		NativeAudio_FreeVoicePcm(&s_audio.voices[v]);
		s_audio.voices[v].sampleDirty = 1;
	}
}

/* True while a voice is still sounding out of this slot; such an entry is the
 * one thing in the store that cannot be reclaimed. */
internal int NativeAudio_HwSampleEntryIsPlaying(const struct NativeAudioHwSampleCacheEntry *entry)
{
	int v;

	for (v = 0; v < NATIVE_AUDIO_SPU_VOICE_COUNT; v++)
	{
		if ((s_audio.voices[v].pcm == entry->pcm) && s_audio.voices[v].active && (s_audio.voices[v].adsrPhase != NATIVE_AUDIO_ADSR_OFF))
		{
			return 1;
		}
	}

	return 0;
}

/* Give an entry's slot back to the store and clear the entry. */
internal void NativeAudio_HwSampleEntryRelease(struct NativeAudioHwSampleCacheEntry *entry)
{
	if (!entry->used)
	{
		return;
	}

	NativeAudio_HwSampleDetachVoices(entry);
	N3DS_HwVoiceStoreFree(entry->pcm);
	memset(entry, 0, sizeof(*entry));
#ifdef CTR_INTERNAL
	s_hwStoreCensus.releases++;
#endif
}

/*
 * Get an entry whose slot holds sampleCount, preferring in order: the stale
 * entry for this same address (a streaming rewrite of the same sample --
 * reusing its slot in place is what keeps the store flat), a never-used
 * entry with a fresh slot, then the smallest stale slot that fits. A
 * same-address entry whose slot has become too small is released rather than
 * stranded, and a store with no room reclaims until it can serve the request,
 * so exhaustion costs a re-decode instead of a silent sound.
 */
internal struct NativeAudioHwSampleCacheEntry *NativeAudio_HwSampleSlotAcquire(u32 addr, int sampleCount)
{
	struct NativeAudioHwSampleCacheEntry *entry = NULL;
	int i;

	for (i = 0; i < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; i++)
	{
		if (!s_hwSampleCache[i].used || !s_hwSampleCache[i].stale || (s_hwSampleCache[i].addr != addr))
		{
			continue;
		}
		if (s_hwSampleCache[i].capacity >= sampleCount)
		{
			entry = &s_hwSampleCache[i];
		}
		else
		{
			NativeAudio_HwSampleEntryRelease(&s_hwSampleCache[i]);
#ifdef CTR_INTERNAL
			s_hwStoreCensus.growReleases++;
#endif
		}
		break;
	}

	if (entry == NULL)
	{
		for (i = 0; i < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; i++)
		{
			s16 *slot;

			if (s_hwSampleCache[i].used)
			{
				continue;
			}

			slot = (s16 *)N3DS_HwVoiceStoreAlloc(sampleCount);
			if (slot == NULL)
			{
				int pass;

				/*
				 * Nothing in the store is pinned except what is sounding right
				 * now, so release the coldest victim and retry, giving up only
				 * as much cached decode as this sample needs: stale entries
				 * first (their content is already invalid), then live entries
				 * no voice is playing (a re-key costs a re-decode).
				 */
				for (pass = 0; (pass < 2) && (slot == NULL); pass++)
				{
					while (slot == NULL)
					{
						struct NativeAudioHwSampleCacheEntry *victim = NULL;
						int j;

						for (j = 0; j < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; j++)
						{
							struct NativeAudioHwSampleCacheEntry *candidate = &s_hwSampleCache[j];

							if (!candidate->used)
							{
								continue;
							}
							if ((pass == 0) ? !candidate->stale : NativeAudio_HwSampleEntryIsPlaying(candidate))
							{
								continue;
							}
							if ((victim == NULL) || (candidate->lastUse < victim->lastUse))
							{
								victim = candidate;
							}
						}
						if (victim == NULL)
						{
							break;
						}
						NativeAudio_HwSampleEntryRelease(victim);
						slot = (s16 *)N3DS_HwVoiceStoreAlloc(sampleCount);
					}
				}
#ifdef CTR_INTERNAL
				s_hwStoreCensus.reclaimRetries++;
#endif
			}
			if (slot == NULL)
			{
				break; /* store exhausted -- try recycling a stale slot below */
			}

#ifdef CTR_INTERNAL
			{
				int d;

				for (d = 0; d < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; d++)
				{
					if (s_hwSampleCache[d].used && (s_hwSampleCache[d].addr == addr))
					{
						s_hwStoreCensus.dupAddr++;
						s_hwStoreCensus.dupBytes += (u32)sampleCount * 2u;
						break;
					}
				}
				s_hwStoreCensus.allocs++;
				s_hwStoreCensus.allocBytes += (u32)sampleCount * 2u;
			}
#endif
			entry = &s_hwSampleCache[i];
			entry->pcm = slot;
			entry->capacity = sampleCount;
			break;
		}
	}

	if (entry == NULL)
	{
		for (i = 0; i < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; i++)
		{
			if (!s_hwSampleCache[i].used || !s_hwSampleCache[i].stale || (s_hwSampleCache[i].capacity < sampleCount))
			{
				continue;
			}
			if ((entry == NULL) || (s_hwSampleCache[i].capacity < entry->capacity))
			{
				entry = &s_hwSampleCache[i];
			}
		}
		if (entry != NULL)
		{
			NativeAudio_HwSampleDetachVoices(entry);
		}
	}

	if (entry != NULL)
	{
		entry->used = 1;
		entry->stale = 0;
		entry->addr = addr;
		entry->lastUse = ++s_hwSampleUseSeq;
	}

	return entry;
}

internal void NativeAudio_HwSampleCacheReset(void)
{
	memset(s_hwSampleCache, 0, sizeof(s_hwSampleCache));
}

/*
 * SpuWrite hit [writeStart, writeEnd). The PS1 technique this must honor
 * is SPU streaming: long audio (the intro announcer, jingles, race-end
 * music) plays by looping a voice over a small buffer whose contents the
 * CPU rewrites continuously. So a write over a CURRENTLY-PLAYING entry is
 * refreshed IN PLACE -- re-decoded into the same slot while the DSP
 * channel keeps looping over it, which reproduces the hardware behavior
 * with no restart, no click, no global invalidation. Only entries the
 * write actually touched are affected at all; everything else keeps
 * playing untouched (v1 reset ALL state on every write, which thrashed
 * the whole cache dozens of times a second during speech -- the session-20
 * "minutes-long race-end freeze").
 */
internal void NativeAudio_HwStreamInvalidateRange(u32 writeStart, u32 writeEnd)
{
	int i;

	for (i = 0; i < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; i++)
	{
		struct NativeAudioHwSampleCacheEntry *entry = &s_hwSampleCache[i];
		u32 entryEnd;
		int playing = 0;
		int v;

		if (!entry->used || entry->stale || (entry->spuBytes == 0))
		{
			continue;
		}
		entryEnd = entry->addr + entry->spuBytes;
		if ((entryEnd <= writeStart) || (entry->addr >= writeEnd))
		{
			continue;
		}

		for (v = 0; v < NATIVE_AUDIO_SPU_VOICE_COUNT; v++)
		{
			if ((s_audio.voices[v].pcm == entry->pcm) && s_audio.voices[v].active && (s_audio.voices[v].adsrPhase != NATIVE_AUDIO_ADSR_OFF))
			{
				playing = 1;
				break;
			}
		}

		if (playing)
		{
			int scanCount;
			int loopStart;
			int loopEnd;
			b32 loopEnabled;

			if (NativeAudio_ScanSpuSampleExtent(entry->addr, 0, &scanCount, &loopStart, &loopEnd, &loopEnabled) &&
			    (scanCount <= entry->capacity))
			{
				struct NativeAudioPcmBuffer pcm;

				memset(&pcm, 0, sizeof(pcm));
				pcm.samples = entry->pcm;
				pcm.capacity = entry->capacity;
				if (NativeAudio_DecodeSpuSample(entry->addr, &pcm))
				{
					N3DS_HwVoiceStoreCommit(entry->pcm, pcm.count);
					entry->count = pcm.count;
					/* Refresh is a complete decode: leave no pending tail for
					 * the pump (a streaming rewrite may land mid-fill). */
					entry->decoded = pcm.count;
					entry->spuBytes = ((u32)(pcm.count + 27) / 28u) * 16u;
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
					s_voiceProf.windowHwRefresh++;
#endif
					/* Voices and channels deliberately untouched: the DSP
					 * loop keeps reading this memory and simply hears the
					 * fresh content -- that IS the streaming semantics. */
					continue;
				}
			}
			/* Couldn't refresh in place (extent grew past the slot, or the
			 * scan failed mid-rewrite) -- fall through to stale. */
		}

		entry->stale = 1;
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
		s_voiceProf.windowHwStaled++;
#endif
		/* Cutting the DSP channel here, not a vblank later at the mirror, is
		 * what lets the slot be reclaimed and refilled safely. */
		NativeAudio_HwSampleDetachVoices(entry);
	}
}

/*
 * Resumable ADPCM fill for one cache entry: the same nibble walk as
 * NativeAudio_DecodeSpuSample, but bounded by maxSamples and carrying the
 * predictor pair in the entry so later calls continue where this one
 * stopped. Produces multiples of 28 (one block) up to the scan-derived
 * count; on any disagreement with the scan it clamps to count and leaves
 * the tail as the zero-fill (silence). Flushes what it produced for the
 * DSP. Caller holds the output lock. Returns samples produced.
 */
internal int NativeAudio_HwDecodeChunk(struct NativeAudioHwSampleCacheEntry *entry, int maxSamples)
{
	int from = entry->decoded;

	while ((entry->decoded < entry->count) && ((entry->decoded - from) < maxSamples))
	{
		u32 block = entry->nextBlock;
		u8 soundParameter;
		u8 flags;
		int i;

		if ((block + 16 > NATIVE_AUDIO_SPU_MEMSIZE) || (entry->decoded + 28 > entry->capacity))
		{
			entry->decoded = entry->count;
			break;
		}

		soundParameter = s_audio.spu.memory[block];
		flags = s_audio.spu.memory[block + 1];

		for (i = 0; i < 14; i++)
		{
			u8 packed = s_audio.spu.memory[block + 2 + i];

			entry->pcm[entry->decoded++] = (s16)NativeAudio_DecodeAdpcmNibble(soundParameter, packed & 0xf, &entry->histOld, &entry->histOlder);
			entry->pcm[entry->decoded++] = (s16)NativeAudio_DecodeAdpcmNibble(soundParameter, (packed >> 4) & 0xf, &entry->histOld, &entry->histOlder);
		}

		entry->nextBlock = block + 16;

		if ((flags & ADPCM_LOOP_END) != 0)
		{
			entry->decoded = entry->count;
			break;
		}
	}

	if (entry->decoded > from)
	{
		N3DS_HwVoiceStoreCommit(entry->pcm + from, entry->decoded - from);
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
		s_voiceProf.windowDecodedSamples += entry->decoded - from;
#endif
	}

	return entry->decoded - from;
}

/*
 * Once per audio-thread wake, or once per vblank from the game thread when
 * there is no audio thread (n3ds_audio_thread.c): finish any entry whose
 * key-on only decoded the head chunk. Budgeted and split into short lock
 * holds -- see the NATIVE_AUDIO_HW_DECODE_* sizing note.
 */
void NativeAudio_HwDecodePumpForAudioThread(void)
{
	int budget = NATIVE_AUDIO_HW_DECODE_PUMP_SAMPLES;

	while (budget > 0)
	{
		struct NativeAudioHwSampleCacheEntry *pending = NULL;
		int produced;
		int chunk;
		int i;

		NativeAudio_LockOutput();
		for (i = 0; i < NATIVE_AUDIO_HW_SAMPLE_CACHE_ENTRIES; i++)
		{
			struct NativeAudioHwSampleCacheEntry *entry = &s_hwSampleCache[i];

			if (entry->used && !entry->stale && (entry->decoded < entry->count))
			{
				pending = entry;
				break;
			}
		}

		if (pending == NULL)
		{
			NativeAudio_UnlockOutput();
			return;
		}

		chunk = (budget < NATIVE_AUDIO_HW_DECODE_CHUNK_SAMPLES) ? budget : NATIVE_AUDIO_HW_DECODE_CHUNK_SAMPLES;
		produced = NativeAudio_HwDecodeChunk(pending, chunk);
		NativeAudio_UnlockOutput();

		if (produced <= 0)
		{
			return;
		}
		budget -= produced;
	}
}
#endif

internal void NativeAudio_ResetVoicePcmArenaNoLock(int markDirty)
{
	int i;

#if CTR_NATIVE_AUDIO_ARENA_LOG
	if (s_audio.voicePcmArena.used > 0)
	{
		Platform_LogWarn("[audio-arena] reset (was %d KiB in use of %d KiB)\n", s_audio.voicePcmArena.used >> 10,
		                 s_audio.voicePcmArena.capacity >> 10);
	}
#endif

	NativeAudio_ArenaReset(&s_audio.voicePcmArena);
	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		int wasActive = s_audio.voices[i].active;

		NativeAudio_FreeVoicePcm(&s_audio.voices[i]);
		if (markDirty && wasActive)
		{
			s_audio.voices[i].sampleDirty = 1;
		}
	}

#if defined(__3DS__)
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
	s_voiceProf.windowHwResets++;
#endif
	/* The linear store's pointers just went stale with the voices'.
	 * StoreReset also silences every hardware channel BEFORE the store is
	 * recycled; safe against the mirror because every caller of this
	 * function holds the render lock the mirror runs under. */
	NativeAudio_HwSampleCacheReset();
	N3DS_HwVoiceStoreReset();
	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		s_hwVoiceShadow[i].started = 0;
	}
#endif
}

// Public entry point so the game side can drop the decoded-PCM cache on paths
// that skip an audio-bank reload. Retail had no host-side decode cache, so the
// PS1 fast-restart never needed an equivalent; on native it is what stops the
// arena growing without bound across repeated Time Trial retries.
//
// markDirty is set so any voice still sounding re-decodes on its next update
// rather than reading through a freed pointer.
void NativeAudio_ResetVoicePcmArena(void)
{
	NativeAudio_LockOutput();
	NativeAudio_ResetVoicePcmArenaNoLock(1);
	NativeAudio_UnlockOutput();
}

/* Defined after NativeAudio_UpdateVoiceSample below (it documents the step-3
 * build split it exists for); declared here for the reclaim guard's use. */
internal int NativeAudio_VoiceHasSample(const struct NativeAudioVoice *voice);

internal void NativeAudio_ReclaimVoicePcmArenaIfIdleNoLock(void)
{
	int i;

	/*
	 * Reset only fires once idle usage exceeds
	 * NATIVE_AUDIO_VOICE_ARENA_RECLAIM_KEEP, not the instant nothing is
	 * sounding: below the high-water mark, decoded PCM survives idle gaps so a
	 * re-triggered sound re-keys for free. The reset still runs -- only ever with no voice live -- once a
	 * genuinely large session pushes past the mark.
	 */
	if (s_audio.voicePcmArena.used <= NATIVE_AUDIO_VOICE_ARENA_RECLAIM_KEEP)
	{
		return;
	}

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		struct NativeAudioVoice *voice = &s_audio.voices[i];

		if (voice->active && NativeAudio_VoiceHasSample(voice) && voice->sampleCount > 0 && voice->adsrPhase != NATIVE_AUDIO_ADSR_OFF)
		{
			return;
		}
	}

	NativeAudio_ResetVoicePcmArenaNoLock(0);
}

/*
 * Reserves exactly sampleCount samples -- the count comes from
 * NativeAudio_ScanSpuSampleExtent, so this no longer books the worst case
 * from addr to the end of SPU RAM (up to ~1.8 MB per key-on, the multi-MB
 * malloc+memcpy spike the lazy decode removes; the
 * 128 MiB arena peak of the old arena-growth bug was this mechanism).
 */
internal int NativeAudio_PrepareVoicePcmBuffer(int sampleCount, struct NativeAudioPcmBuffer *pcm, int *markerOut)
{
	int bytes;
	int marker;

	if ((sampleCount <= 0) || (sampleCount > (INT_MAX / (int)sizeof(s16))))
	{
		return 0;
	}

	bytes = sampleCount * (int)sizeof(s16);
	marker = NativeAudio_AlignUpInt(s_audio.voicePcmArena.used, NATIVE_AUDIO_ARENA_ALIGN);
	if ((marker < 0) || (bytes > INT_MAX - marker))
	{
		return 0;
	}

	if (!NativeAudio_VoiceArenaEnsureCapacityNoLock(marker + bytes))
	{
		return 0;
	}

	pcm->samples = (s16 *)NativeAudio_ArenaPush(&s_audio.voicePcmArena, bytes, NATIVE_AUDIO_ARENA_ALIGN, markerOut);
	if (pcm->samples == NULL)
	{
		return 0;
	}

	pcm->count = 0;
	pcm->capacity = sampleCount;
	return 1;
}


internal void NativeAudio_WrapVoiceLoop(struct NativeAudioVoice *voice)
{
	u64 loopStartFp = (u64)voice->loopStart << NATIVE_AUDIO_FP_SHIFT;
	u64 loopEndFp = (u64)voice->loopEnd << NATIVE_AUDIO_FP_SHIFT;
	u64 loopLenFp = loopEndFp - loopStartFp;
	u64 overshoot = voice->positionFp - loopEndFp;

	voice->positionFp = loopStartFp + (loopLenFp != 0 ? overshoot % loopLenFp : 0);
	voice->looped = 1;
}

internal int NativeAudio_UpdateVoiceSample(struct NativeAudioVoice *voice)
{
	struct NativeAudioPcmBuffer pcm;
	int scanCount;
	int loopStart;
	int loopEnd;
	b32 loopEnabled;
	int arenaMarker = 0;

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
	s_voiceProf.windowDecodeCalls++;
#endif

	/* Metadata first: exact extent + loop points, ~1/28th of a decode. */
	if (!NativeAudio_ScanSpuSampleExtent(voice->attr.addr, voice->attr.loop_addr, &scanCount, &loopStart, &loopEnd, &loopEnabled))
	{
		NativeAudio_FreeVoicePcm(voice);
		voice->active = 0;
		voice->sampleDirty = 0;
		return 0;
	}

#if defined(__3DS__)
	/*
	 * Hardware voices: decode ONCE per (addr, loop_addr) into the
	 * DSP-readable linear store; every later key-on of the same sample is
	 * a table hit, which is what deletes the audio_voice_start stalls the
	 * session-20 hardware run measured (7-15ms/frame). SpuWrite resets
	 * both table and store via NativeAudio_ResetVoicePcmArenaNoLock.
	 */
	{
		struct NativeAudioHwSampleCacheEntry *entry = NativeAudio_HwSampleCacheLookup(voice->attr.addr);

		(void)arenaMarker;
		(void)pcm;
		if (entry == NULL)
		{
			entry = NativeAudio_HwSampleSlotAcquire(voice->attr.addr, scanCount);
			if (entry == NULL)
			{
				/* Store and table both exhausted: same failure shape as an
				 * arena-exhausted decode on PC, and one lost sound. */
				s_hwStoreLostKeyOns++;
				NativeAudio_FreeVoicePcm(voice);
				voice->active = 0;
				voice->sampleDirty = 0;
				return 0;
			}

			entry->count = scanCount;
			entry->spuBytes = ((u32)(scanCount + 27) / 28u) * 16u;
			entry->decoded = 0;
			entry->nextBlock = voice->attr.addr;
			entry->histOld = 0;
			entry->histOlder = 0;

			/* Zero-fill before the head chunk so anything reading past the
			 * decode cursor -- recycled slot content included -- hears
			 * silence. One whole-range flush covers zeros and head both;
			 * the pump flushes its own chunks as they land. */
			memset(entry->pcm, 0, (size_t)scanCount * sizeof(s16));
			NativeAudio_HwDecodeChunk(entry, N3DS_AudioThreadIsRunning() ? NATIVE_AUDIO_HW_DECODE_HEAD_SAMPLES : NATIVE_AUDIO_HW_DECODE_HEAD_SAMPLES_INLINE);
			N3DS_HwVoiceStoreCommit(entry->pcm, scanCount);

			NativeAudio_FreeVoicePcm(voice);
			voice->pcm = entry->pcm;
			voice->sampleCount = entry->count;
		}
		else
		{
			NativeAudio_FreeVoicePcm(voice);
			voice->pcm = entry->pcm;
			voice->sampleCount = entry->count;
		}

		/* Loop fields come from the scan just above either way -- for a
		 * cache hit they are byte-identical to the entry's (same SPU
		 * content between uploads decodes identically). */
		voice->loopStart = loopStart;
		voice->loopEnd = loopEnd;
		voice->loopEnabled = loopEnabled;
		voice->sampleDirty = 0;
		return 1;
	}
#else
	memset(&pcm, 0, sizeof(pcm));
	if (!NativeAudio_PrepareVoicePcmBuffer(scanCount, &pcm, &arenaMarker))
	{
		NativeAudio_ResetVoicePcmArenaNoLock(1);
		if (!NativeAudio_PrepareVoicePcmBuffer(scanCount, &pcm, &arenaMarker))
		{
			NativeAudio_FreeVoicePcm(voice);
			voice->active = 0;
			voice->sampleDirty = 0;
			return 0;
		}
	}

	if (!NativeAudio_DecodeSpuSample(voice->attr.addr, &pcm))
	{
		NativeAudio_ArenaRewind(&s_audio.voicePcmArena, arenaMarker);
		NativeAudio_FreeVoicePcm(voice);
		voice->active = 0;
		voice->sampleDirty = 0;
		return 0;
	}

	/* Exact reservation makes this a no-op when the scan and the decode
	 * agree (they walk identical block logic); kept as the backstop. */
	NativeAudio_ArenaRewind(&s_audio.voicePcmArena, arenaMarker + pcm.count * (int)sizeof(s16));

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
	s_voiceProf.windowDecodedSamples += pcm.count;
#endif

	NativeAudio_FreeVoicePcm(voice);
	voice->pcm = pcm.samples;
	voice->sampleCount = pcm.count;
	voice->loopStart = loopStart;
	voice->loopEnd = loopEnd;
	voice->loopEnabled = loopEnabled;
	voice->sampleDirty = 0;
	return 1;
#endif
}

/*
 * "Has a decoded sample" predicate for every site that used to test
 * voice->pcm != NULL directly. Kept as a named predicate (rather than
 * reverting the call sites) because it earned its name once already: the
 * muted 3DS build's metadata-only fast path rode on it for a whole
 * development phase.
 */
internal int NativeAudio_VoiceHasSample(const struct NativeAudioVoice *voice)
{
	return voice->pcm != NULL;
}

/* What NativeAudio_FreeXA hands back for the deferred close: the source
 * handle plus whether it was open -- nothing else survives the free. It
 * used to be the whole stream struct, which was genuinely "a small struct
 * copy" at ~100 bytes and silently became a 37KB stack object per caller
 * when the decode scratch moved into the stream (same as
 * NativeAudio_XAStreamSwap). */
struct NativeAudioXaClosedSource
{
	struct NativeAudioXaSource source;
	b32 open;
};

// NOTE(aalhendi): Assumes the caller already holds NativeAudio_LockOutput(),
// exactly like the old memset-only version did -- every existing call site
// (StopXA, PlayXATrack, PlayXAFile, RestoreState) already calls this from
// inside its own lock/unlock bracket. Captures and clears the live stream
// in place and hands the source back so the caller can close its underlying
// file handle *after* unlocking, per the plan's "expensive/blocking work
// outside the lock" rule -- fclose() while holding the SDL audio lock would
// block the mixer thread on file-system I/O.
internal struct NativeAudioXaClosedSource NativeAudio_FreeXA(void)
{
	struct NativeAudioXaClosedSource closed;

	closed.source = s_audio.xaStream.source;
	closed.open = s_audio.xaStream.open;
	NativeAudio_XAStreamClearNoLock(&s_audio.xaStream);
	memset(&s_audio.xa, 0, sizeof(s_audio.xa));

	return closed;
}

internal void NativeAudio_CopyVoiceToState(struct NativeAudioVoiceState *dst, const struct NativeAudioVoice *src)
{
	dst->attr = src->attr;
	dst->sampleCount = src->sampleCount;
	dst->loopStart = src->loopStart;
	dst->loopEnd = src->loopEnd;
	dst->loopEnabled = src->loopEnabled;
	dst->active = src->active;
	dst->positionFp = src->positionFp;
	dst->looped = src->looped;
	dst->reverb = src->reverb;
	dst->sampleDirty = src->sampleDirty;
	dst->adsrLevel = src->adsrLevel;
	dst->adsrCounter = src->adsrCounter;
	dst->adsrPhase = src->adsrPhase;
}

internal void NativeAudio_CopyStateToVoice(struct NativeAudioVoice *dst, const struct NativeAudioVoiceState *src)
{
	dst->attr = src->attr;
	dst->sampleCount = 0;
	dst->loopStart = 0;
	dst->loopEnd = 0;
	dst->loopEnabled = 0;
	dst->active = src->active;
	dst->positionFp = src->positionFp;
	dst->looped = src->looped;
	dst->reverb = src->reverb;
	dst->sampleDirty = src->sampleDirty || src->active;
	dst->adsrLevel = src->adsrLevel;
	dst->adsrCounter = src->adsrCounter;
	dst->adsrPhase = src->adsrPhase;
	dst->pcm = NULL;
}

internal void NativeAudio_CopyXAToState(struct NativeAudioXAState *dst, const struct NativeAudioXA *src)
{
	dst->frameCount = src->frameCount;
	dst->sampleRate = src->sampleRate;
	dst->categoryID = src->categoryID;
	dst->xaID = src->xaID;
	dst->hasTrackIdentity = src->hasTrackIdentity;
	dst->active = src->active;
	dst->positionFp = src->positionFp;
	dst->outputFrame = src->outputFrame;
	dst->stepFp = src->stepFp;
	dst->volumeLeft = src->volumeLeft;
	dst->volumeRight = src->volumeRight;
}

internal int NativeAudio_ValidateVoiceSnapshot(const struct NativeAudioVoiceState *voice)
{
	if (voice->active && (voice->attr.addr >= NATIVE_AUDIO_SPU_MEMSIZE))
	{
		return 0;
	}
	if (voice->attr.loop_addr >= NATIVE_AUDIO_SPU_MEMSIZE)
	{
		return 0;
	}
	if (voice->adsrPhase > NATIVE_AUDIO_ADSR_RELEASE)
	{
		return 0;
	}
	if ((voice->adsrLevel < NATIVE_AUDIO_ADSR_MIN) || (voice->adsrLevel > NATIVE_AUDIO_ADSR_MAX))
	{
		return 0;
	}
	return 1;
}

internal int NativeAudio_ValidateXASnapshot(const struct NativeAudioXAState *xa)
{
	u64 outputFrameCount;

	if ((xa->active != 0) && (xa->active != 1))
	{
		return 0;
	}
	if ((xa->hasTrackIdentity != 0) && (xa->hasTrackIdentity != 1))
	{
		return 0;
	}
	if ((xa->frameCount < 0) || (xa->sampleRate < 0))
	{
		return 0;
	}
	if (!xa->active && !xa->hasTrackIdentity)
	{
		return 1;
	}
	if ((xa->categoryID < 0) || (xa->categoryID >= XA_NUM_TYPES))
	{
		return 0;
	}
	if (xa->xaID < 0)
	{
		return 0;
	}
	if (xa->active && ((xa->frameCount <= 0) || (xa->sampleRate <= 0)))
	{
		return 0;
	}
	outputFrameCount = NativeAudio_GetXAOutputFrameCount(xa->frameCount, xa->sampleRate);
	if (xa->active && ((outputFrameCount == 0) || (xa->outputFrame >= outputFrameCount)))
	{
		return 0;
	}
	if (xa->active && ((xa->positionFp >> NATIVE_AUDIO_FP_SHIFT) >= (u64)xa->frameCount))
	{
		return 0;
	}
	return 1;
}

internal int NativeAudio_ValidateReverbSnapshot(const struct NativeAudioReverbState *reverb)
{
	if ((reverb->mode < SPU_REV_MODE_OFF) || (reverb->mode >= SPU_REV_MODE_MAX))
	{
		return 0;
	}
	if ((reverb->sizeSamples < 0) || (reverb->sizeSamples > NATIVE_AUDIO_REVERB_MAX_SAMPLES))
	{
		return 0;
	}
	if ((reverb->inputHistoryCursor < 0) || (reverb->inputHistoryCursor >= NATIVE_AUDIO_REVERB_FIR_TAPS))
	{
		return 0;
	}
	if ((reverb->outputHistoryCursor < 0) || (reverb->outputHistoryCursor >= NATIVE_AUDIO_REVERB_FIR_TAPS))
	{
		return 0;
	}
	if ((reverb->samplePhase < 0) || (reverb->samplePhase > 1))
	{
		return 0;
	}
	if (reverb->sizeSamples == 0)
	{
		return reverb->cursor == 0;
	}
	if ((reverb->cursor < 0) || (reverb->cursor >= reverb->sizeSamples))
	{
		return 0;
	}
	return 1;
}

internal void NativeAudio_ClearOutputQueueNoLock(void)
{
	s_audio.output.scheduledReadFrame = 0;
	s_audio.output.scheduledFrameCount = 0;

	if (s_audio.output.stream != NULL)
	{
		SDL_ClearAudioStream(s_audio.output.stream);
	}
}

internal int NativeAudio_OpenDevice(void);
internal void NativeAudio_MixFrame(s16 *outLeft, s16 *outRight);
internal int NativeAudio_RenderFramesNoLock(s16 *out, int frameCount);

internal void NativeAudio_SelectDriverHint(void)
{
#if defined(__linux__)
	if (SDL_GetHint(SDL_HINT_AUDIO_DRIVER) == NULL)
	{
		// NOTE(aalhendi): Keep native Linux playback on the SDL3 drivers that
		// were stable in live probes. User/env overrides still win, and
		// PipeWire remains available as a fallback.
		SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,alsa,pipewire");
	}
#endif
	if (SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) == NULL)
	{
		SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
	}
}

internal int NativeAudio_BuildXAPath(char *path, size_t pathSize, int categoryID, int fileNumber)
{
	const char *dir = NULL;
	int written;

	if (categoryID == 0)
	{
		dir = "XA/MUSIC";
	}
	else if (categoryID == 1)
	{
		dir = "XA/ENG/EXTRA";
	}
	else if (categoryID == 2)
	{
		dir = "XA/ENG/GAME";
	}
	else
	{
		return 0;
	}

	written = snprintf(path, pathSize, "%s/S%02d.XA", dir, fileNumber);
	return (written > 0) && ((size_t)written < pathSize);
}

internal int NativeAudio_LookupPalVoiceTrackInfo(int categoryID, int xaID, struct NativeAudioXaTrackInfo *info);

internal int NativeAudio_LookupXATrackInfo(int categoryID, int xaID, struct NativeAudioXaTrackInfo *info)
{
	struct NativeAudioByteBuffer xnf;
	int numXasTotal;
	int numTracksTotal;
	int xaSizeOffset;
	int xaSizeEnd;
	int numSongs;
	int firstSongIndex;
	int entryIndex;
	const u8 *entry;

	if ((categoryID < 0) || (categoryID >= XA_NUM_TYPES) || (xaID < 0))
	{
		return 0;
	}

#if defined(__3DS__)
	/* A PAL voice pack is selected: speech (EXTRA, GAME) comes from it when it has the line.
	 * Anything it lacks falls through to the disc's English below. */
	if (NativeAudio_LookupPalVoiceTrackInfo(categoryID, xaID, info))
	{
		return 1;
	}
#endif

	if (!NativeAudio_ReadXnfBytes(&xnf))
	{
		return 0;
	}

	if (xnf.size < XA_HEADER_SIZE)
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	if ((NativeAudio_ReadLE32(&xnf.data[0]) != 0x464e4958) || (NativeAudio_ReadLE32(&xnf.data[4]) != 102) ||
	    (NativeAudio_ReadLE32(&xnf.data[8]) != XA_NUM_TYPES))
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	numXasTotal = NativeAudio_ReadLE32(&xnf.data[XA_NUM_XAS_TOTAL_OFFSET]);
	numTracksTotal = NativeAudio_ReadLE32(&xnf.data[XA_NUM_TRACKS_TOTAL_OFFSET]);
	if ((numXasTotal < 0) || (numTracksTotal < 0) || (numXasTotal > ((INT_MAX - XA_HEADER_SIZE) / 4)))
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	xaSizeOffset = XA_HEADER_SIZE + numXasTotal * 4;
	if (numTracksTotal > ((INT_MAX - xaSizeOffset) / XA_SIZE_ENTRY_BYTES))
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	xaSizeEnd = xaSizeOffset + numTracksTotal * XA_SIZE_ENTRY_BYTES;
	if ((xaSizeEnd < xaSizeOffset) || (xaSizeEnd > xnf.size))
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	numSongs = NativeAudio_ReadLE32(&xnf.data[XA_NUM_SONGS_OFFSET + categoryID * 4]);
	firstSongIndex = NativeAudio_ReadLE32(&xnf.data[XA_FIRST_SONG_INDEX_OFFSET + categoryID * 4]);
	if (xaID >= numSongs)
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	entryIndex = firstSongIndex + xaID;
	if ((entryIndex < 0) || (entryIndex >= numTracksTotal))
	{
		NativeAudio_FreeByteBuffer(&xnf);
		return 0;
	}

	entry = &xnf.data[xaSizeOffset + entryIndex * XA_SIZE_ENTRY_BYTES];
	info->channelFilter = entry[0];
	info->fileNumber = entry[1];
	info->numSectors = NativeAudio_ReadLE16Signed(entry + 2);
	info->entryIndex = entryIndex;
	info->pack = 0;

	NativeAudio_FreeByteBuffer(&xnf);
	return info->numSectors > 0;
}


#if defined(__3DS__)
/*
 * PAL voice packs (3DS).
 *
 * The NTSC-U disc only carries English speech. A pack extracted from a PAL disc with
 * tools/extract_pal_voices.py, placed in <base dir>/mods/pal-voices (on the SD card:
 * /3ds/ctr_native/mods/pal-voices), gives the other languages their own voices:
 *
 *   mods/pal-voices/XA/<CODE>.XNF              the pack's track table
 *   mods/pal-voices/XA/<CODE>/EXTRA/Sxx.XA     raw 2352-byte sectors, copied off the disc unchanged
 *   mods/pal-voices/XA/<CODE>/GAME/Sxx.XA      (CODE = ENG FRN GRM ITL SPN DCH)
 *
 * LOAD_LangFile reports the language (NativeAudio_SetVoiceLanguage). While a pack is
 * selected, every EXTRA/GAME lookup resolves through the pack's own XNF; a line the pack does not
 * have (or a pack that is not installed) plays from the disc's English exactly as before. MUSIC
 * is never touched. Nothing here runs unless a pack is selected, so English is unchanged.
 *
 * Voice pack numbers are LOAD_LangFile's language file index minus one:
 * 2 en2 -> 1 ENG, 3 fr -> 2 FRN, 4 de -> 3 GRM, 5 it -> 4 ITL, 6 es -> 5 SPN, 7 nl -> 6 DCH.
 */
#define NATIVE_AUDIO_PAL_PACK_COUNT    6
#define NATIVE_AUDIO_PAL_FILE_MAX      256
#define NATIVE_AUDIO_PAL_XNF_MAX_BYTES (256 * 1024)
#define NATIVE_AUDIO_PAL_KEY_STRIDE    512 /* must equal NATIVE_AUDIO_XA_TRACK_INDEX_MAX (asserted below it) */

__attribute__((format(printf, 1, 2))) internal void NativeAudio_XAVoiceCacheLog(const char *fmt, ...);

internal int s_voicePack; /* 0 = disc voices */
internal struct NativeAudioByteBuffer s_palXnf[NATIVE_AUDIO_PAL_PACK_COUNT + 1];
internal u8 s_palXnfTried[NATIVE_AUDIO_PAL_PACK_COUNT + 1];
/* [pack][category - 1][file number]: 0 = not looked at yet, 1 = present, 2 = missing. One SD probe per file per boot. */
internal u8 s_palFileState[NATIVE_AUDIO_PAL_PACK_COUNT + 1][2][NATIVE_AUDIO_PAL_FILE_MAX];

internal const char *NativeAudio_PalPackCode(int pack)
{
	static const char *const codes[NATIVE_AUDIO_PAL_PACK_COUNT + 1] = {NULL, "ENG", "FRN", "GRM", "ITL", "SPN", "DCH"};

	return ((pack >= 1) && (pack <= NATIVE_AUDIO_PAL_PACK_COUNT)) ? codes[pack] : NULL;
}

/* fileNumber < 0 names the pack's XNF manifest. */
internal int NativeAudio_PalVoicePath(char *path, size_t pathSize, int pack, int categoryID, int fileNumber)
{
	const char *code = NativeAudio_PalPackCode(pack);
	const char *category;
	char relative[96];
	int written;

	if (code == NULL)
	{
		return 0;
	}

	if (fileNumber < 0)
	{
		written = snprintf(relative, sizeof(relative), "mods/pal-voices/XA/%s.XNF", code);
	}
	else
	{
		if (categoryID == 1)
		{
			category = "EXTRA";
		}
		else if (categoryID == 2)
		{
			category = "GAME";
		}
		else
		{
			return 0;
		}
		written = snprintf(relative, sizeof(relative), "mods/pal-voices/XA/%s/%s/S%02d.XA", code, category, fileNumber);
	}

	if ((written <= 0) || ((size_t)written >= sizeof(relative)))
	{
		return 0;
	}

	return NativePath_Join(path, pathSize, NativeStr8_FromCString(NativeAssets_GetBaseDir()), NativeStr8_FromCString(relative));
}

/* The pack's XNF, read from the SD card once and kept. NULL if the pack is not installed. */
internal const struct NativeAudioByteBuffer *NativeAudio_GetPalXnf(int pack)
{
	if ((pack < 1) || (pack > NATIVE_AUDIO_PAL_PACK_COUNT))
	{
		return NULL;
	}

	if (!s_palXnfTried[pack])
	{
		char path[256];
		FILE *file;

		s_palXnfTried[pack] = 1;
		if (NativeAudio_PalVoicePath(path, sizeof(path), pack, 0, -1) && ((file = fopen(path, "rb")) != NULL))
		{
			long size = 0;

			if ((fseek(file, 0, SEEK_END) == 0) && ((size = ftell(file)) >= XA_HEADER_SIZE) && (size <= NATIVE_AUDIO_PAL_XNF_MAX_BYTES) && (fseek(file, 0, SEEK_SET) == 0))
			{
				u8 *data = (u8 *)malloc((size_t)size);

				if ((data != NULL) && (fread(data, 1, (size_t)size, file) == (size_t)size))
				{
					s_palXnf[pack].data = data;
					s_palXnf[pack].size = (int)size;
				}
				else
				{
					free(data);
				}
			}
			fclose(file);
		}

		if (s_palXnf[pack].data != NULL)
		{
			NativeAudio_XAVoiceCacheLog("[PALVOICE] pack %s found", NativeAudio_PalPackCode(pack));
		}
		else
		{
			NativeAudio_XAVoiceCacheLog("[PALVOICE] pack %s not installed (needs mods/pal-voices/XA/%s.XNF) -- English voices", NativeAudio_PalPackCode(pack),
			                            NativeAudio_PalPackCode(pack));
		}
	}

	return (s_palXnf[pack].data != NULL) ? &s_palXnf[pack] : NULL;
}

internal int NativeAudio_PalTrackFileExists(int pack, int categoryID, int fileNumber)
{
	u8 *state;

	if ((pack < 1) || (pack > NATIVE_AUDIO_PAL_PACK_COUNT) || (categoryID < 1) || (categoryID > 2) || (fileNumber < 0) || (fileNumber >= NATIVE_AUDIO_PAL_FILE_MAX))
	{
		return 0;
	}

	state = &s_palFileState[pack][categoryID - 1][fileNumber];
	if (*state == 0)
	{
		char path[256];
		FILE *file;

		*state = 2;
		if (NativeAudio_PalVoicePath(path, sizeof(path), pack, categoryID, fileNumber) && ((file = fopen(path, "rb")) != NULL))
		{
			fclose(file);
			*state = 1;
		}
		else
		{
			NativeAudio_XAVoiceCacheLog("[PALVOICE] %s cat=%d S%02d.XA missing -- English for its lines", NativeAudio_PalPackCode(pack), categoryID, fileNumber);
		}
	}

	return *state == 1;
}

/* Same XNF layout as the disc's, parsed straight from a byte range. */
internal int NativeAudio_ParseXnfEntry(const u8 *data, int size, int categoryID, int xaID, struct NativeAudioXaTrackInfo *info)
{
	int numXasTotal;
	int numTracksTotal;
	int xaSizeOffset;
	int xaSizeEnd;
	int numSongs;
	int firstSongIndex;
	int entryIndex;
	const u8 *entry;

	if ((data == NULL) || (info == NULL) || (categoryID < 0) || (categoryID >= XA_NUM_TYPES) || (xaID < 0) || (size < XA_HEADER_SIZE))
	{
		return 0;
	}
	if ((NativeAudio_ReadLE32(&data[0]) != 0x464e4958) || (NativeAudio_ReadLE32(&data[4]) != 102) || (NativeAudio_ReadLE32(&data[8]) != XA_NUM_TYPES))
	{
		return 0;
	}

	numXasTotal = NativeAudio_ReadLE32(&data[XA_NUM_XAS_TOTAL_OFFSET]);
	numTracksTotal = NativeAudio_ReadLE32(&data[XA_NUM_TRACKS_TOTAL_OFFSET]);
	if ((numXasTotal < 0) || (numTracksTotal < 0) || (numXasTotal > ((INT_MAX - XA_HEADER_SIZE) / 4)))
	{
		return 0;
	}
	xaSizeOffset = XA_HEADER_SIZE + numXasTotal * 4;
	if (numTracksTotal > ((INT_MAX - xaSizeOffset) / XA_SIZE_ENTRY_BYTES))
	{
		return 0;
	}
	xaSizeEnd = xaSizeOffset + numTracksTotal * XA_SIZE_ENTRY_BYTES;
	if ((xaSizeEnd < xaSizeOffset) || (xaSizeEnd > size))
	{
		return 0;
	}

	numSongs = NativeAudio_ReadLE32(&data[XA_NUM_SONGS_OFFSET + categoryID * 4]);
	firstSongIndex = NativeAudio_ReadLE32(&data[XA_FIRST_SONG_INDEX_OFFSET + categoryID * 4]);
	if (xaID >= numSongs)
	{
		return 0;
	}
	entryIndex = firstSongIndex + xaID;
	if ((entryIndex < 0) || (entryIndex >= numTracksTotal))
	{
		return 0;
	}

	entry = &data[xaSizeOffset + entryIndex * XA_SIZE_ENTRY_BYTES];
	info->channelFilter = entry[0];
	info->fileNumber = entry[1];
	info->numSectors = NativeAudio_ReadLE16Signed(entry + 2);
	info->entryIndex = entryIndex;
	info->pack = 0;
	return info->numSectors > 0;
}

/*
 * Where each disc (NTSC-U) speech line lives in a PAL pack, or -1 if the PAL disc has no such
 * line. PAL dropped some lines and renumbered the rest, so the ID cannot be used as is. This is
 * the mapping the PS Vita port (Rinnegatamante/Crash-Team-Racing-High-Octane) uses.
 */
internal int NativeAudio_ResolvePalVoiceXaID(int categoryID, int xaID)
{
	if (xaID < 0)
	{
		return -1;
	}

	if (categoryID == 1)
	{
		// PAL EXTRA removes NTSC-U 0x3e/0x3f and 0x53..0x56. Everything
		// after the first removed pair therefore moves down by two slots.
		if (xaID <= 0x3d)
		{
			return xaID;
		}
		if ((xaID >= 0x40) && (xaID <= 0x52))
		{
			return xaID - 2;
		}
		return -1;
	}

	if (categoryID == 2)
	{
		if (xaID <= 0x12f)
		{
			int characterID = xaID / 19;
			int characterVoiceIndex = xaID % 19;

			if ((characterID >= 0) && (characterID <= 15) && (characterID != 10) && (characterVoiceIndex < 16))
			{
				return characterID * 16 + characterVoiceIndex;
			}

			if ((xaID >= 0xc0) && (xaID <= 0xcd))
			{
				return xaID - 0x1e;
			}
			return -1;
		}

		if (xaID == 0x130)
		{
			return 0x100;
		}
		if (xaID == 0x131)
		{
			return 0x101;
		}
		if ((xaID >= 0x134) && (xaID <= 0x139))
		{
			return xaID - 0x32;
		}
		return -1;
	}

	return -1;
}

/* The selected pack's version of a speech line: 1 and fills `info`, or 0 to use the disc's English. */
internal int NativeAudio_LookupPalVoiceTrackInfo(int categoryID, int xaID, struct NativeAudioXaTrackInfo *info)
{
	const struct NativeAudioByteBuffer *xnf;
	int pack = s_voicePack;
	int palXaID;

	if ((pack < 1) || (pack > NATIVE_AUDIO_PAL_PACK_COUNT) || ((categoryID != 1) && (categoryID != 2)))
	{
		return 0;
	}

	palXaID = NativeAudio_ResolvePalVoiceXaID(categoryID, xaID);
	if (palXaID < 0)
	{
		return 0;
	}

	xnf = NativeAudio_GetPalXnf(pack);
	if ((xnf == NULL) || !NativeAudio_ParseXnfEntry(xnf->data, xnf->size, categoryID, palXaID, info))
	{
		return 0;
	}

	if ((info->entryIndex >= NATIVE_AUDIO_PAL_KEY_STRIDE) || !NativeAudio_PalTrackFileExists(pack, categoryID, info->fileNumber))
	{
		return 0;
	}

	info->entryIndex += pack * NATIVE_AUDIO_PAL_KEY_STRIDE;
	info->pack = pack;
	return 1;
}

/* Called from LOAD_LangFile with the language file index that was just loaded. */
void NativeAudio_SetVoiceLanguage(int languageFileIndex)
{
	int pack = ((languageFileIndex >= 2) && (languageFileIndex <= 7)) ? (languageFileIndex - 1) : 0;

	s_voicePack = pack;
	if (pack != 0)
	{
		/* Read the manifest now, at the menu, rather than on the first voice line mid-race. */
		(void)NativeAudio_GetPalXnf(pack);
	}
}

/* 1 while the selected language has an installed voice pack. */
int NativeAudio_HasPalVoicePack(void)
{
	return (s_voicePack != 0) && (NativeAudio_GetPalXnf(s_voicePack) != NULL);
}

/* The pack whose lines a lookup returns while `pack` is selected: a pack that is not installed plays
 * the disc's English (0). */
internal int NativeAudio_EffectiveVoicePack(int pack)
{
	return ((pack != 0) && (NativeAudio_GetPalXnf(pack) != NULL)) ? pack : 0;
}

/* Reads (or learns the absence of) the voice manifest a language would select, without selecting
 * it, so a later language switch costs no SD access. Game thread, before the voice cache warm. */
void NativeAudio_PreloadVoiceManifest(int languageFileIndex)
{
	int pack = ((languageFileIndex >= 2) && (languageFileIndex <= 7)) ? (languageFileIndex - 1) : 0;

	if (pack != 0)
	{
		(void)NativeAudio_GetPalXnf(pack);
	}
}

internal void NativeAudio_PalVoiceShutdown(void)
{
	for (int pack = 0; pack <= NATIVE_AUDIO_PAL_PACK_COUNT; pack++)
	{
		NativeAudio_FreeByteBuffer(&s_palXnf[pack]);
		s_palXnfTried[pack] = 0;
	}
	memset(s_palFileState, 0, sizeof(s_palFileState));
}
#else
void NativeAudio_SetVoiceLanguage(int languageFileIndex)
{
	(void)languageFileIndex;
}

int NativeAudio_HasPalVoicePack(void)
{
	return 0;
}

void NativeAudio_PreloadVoiceManifest(int languageFileIndex)
{
	(void)languageFileIndex;
}
#endif /* __3DS__ */

internal int NativeAudio_GetXASectorLayout(int byteCount, int *sectorSizeOut, int *sectorBaseOut, int *totalSectorsOut)
{
	int sectorSize;

	if (byteCount <= 0)
	{
		return 0;
	}

	if ((byteCount % XA_FULL_SECTOR_SIZE) == 0)
	{
		sectorSize = XA_FULL_SECTOR_SIZE;
	}
	else if ((byteCount % XA_FORM2_SECTOR_SIZE) == 0)
	{
		sectorSize = XA_FORM2_SECTOR_SIZE;
	}
	else
	{
		return 0;
	}

	*sectorSizeOut = sectorSize;
	*sectorBaseOut = (sectorSize == XA_FULL_SECTOR_SIZE) ? 16 : 0;
	*totalSectorsOut = byteCount / sectorSize;
	return *totalSectorsOut > 0;
}

internal int NativeAudio_IsXAAudioSector(const u8 *sector, int sectorBase, int channelFilter)
{
	const u8 *header = &sector[sectorBase];
	int subMode = header[2];
	int coding = header[3];
	int bpsBits = (coding >> 4) & 0x03;

	return ((subMode & 0x04) != 0) && (header[0] == 1) && (header[1] == channelFilter) && (bpsBits == 0);
}

internal int NativeAudio_DecodeXA28Nibbles(const u8 *sector, int frameOff, int block, int nibble, int channel, struct NativeAudioXaDecodeState *state,
                                           struct NativeAudioPcmBuffer *out)
{
	int param = sector[frameOff + 4 + block * 2 + nibble];
	int shift = param & 0xf;
	int weight = (param >> 4) & 0xf;
	int w0;
	int w1;
	int i;

	if (weight > 4)
	{
		weight = 4;
	}

	w0 = s_posTable[weight];
	w1 = s_negTable[weight];

	for (i = 0; i < XA_SAMPLES_PER_SOUND_UNIT; i++)
	{
		u8 byte = sector[frameOff + 16 + i * 4 + block];
		u8 nib = (nibble == 0) ? (u8)((byte & 0xf) << 4) : (u8)(byte & 0xf0);
		int sample = ((s8)nib) << 8;

		sample >>= shift;
		sample += (state->old[channel] * w0) >> 6;
		sample += (state->older[channel] * w1) >> 6;
		sample = NativeAudio_Clamp16(sample);

		state->older[channel] = state->old[channel];
		state->old[channel] = sample;
		if (!NativeAudio_PcmPush(out, (s16)sample))
		{
			return 0;
		}
	}

	return 1;
}

internal int NativeAudio_DecodeXASectorMono(const u8 *sector, int sectorBase, struct NativeAudioXaDecodeState *state, struct NativeAudioPcmBuffer *out)
{
	int frame;

	for (frame = 0; frame < XA_FRAMES_PER_SECTOR; frame++)
	{
		int frameOff = sectorBase + XA_SUBHEADER_SIZE + frame * XA_FRAME_SIZE;
		const u8 *header = &sector[frameOff + 4];
		int su;

		for (su = 0; su < XA_SUBFRAMES_PER_FRAME; su++)
		{
			int paramIndex = (su & 3) | ((su & 4) << 1);
			int param = header[paramIndex];
			int shift = param & 0xf;
			int weight = (param >> 4) & 0xf;
			int w0;
			int w1;
			int i;

			if (weight > 4)
			{
				weight = 4;
			}

			w0 = s_posTable[weight];
			w1 = s_negTable[weight];

			for (i = 0; i < XA_SAMPLES_PER_SOUND_UNIT; i++)
			{
				u8 byte = sector[frameOff + 16 + i * 4 + (su >> 1)];
				u8 nib = ((su & 1) == 0) ? (u8)((byte & 0xf) << 4) : (u8)(byte & 0xf0);
				int sample = ((s8)nib) << 8;

				sample >>= shift;
				sample += (state->old[0] * w0) >> 6;
				sample += (state->older[0] * w1) >> 6;
				sample = NativeAudio_Clamp16(sample);

				state->older[0] = state->old[0];
				state->old[0] = sample;
				if (!NativeAudio_PcmPush(out, (s16)sample))
				{
					return 0;
				}
			}
		}
	}

	return 1;
}

internal int NativeAudio_DecodeXASectorStereo(const u8 *sector, int sectorBase, struct NativeAudioXaDecodeState *state, struct NativeAudioPcmBuffer *out)
{
	int frame;

	for (frame = 0; frame < XA_FRAMES_PER_SECTOR; frame++)
	{
		int frameOff = sectorBase + XA_SUBHEADER_SIZE + frame * XA_FRAME_SIZE;
		int block;

		for (block = 0; block < XA_BLOCKS_PER_FRAME; block++)
		{
			s16 left[XA_SAMPLES_PER_SOUND_UNIT];
			s16 right[XA_SAMPLES_PER_SOUND_UNIT];
			s16 tmpSamples[XA_SAMPLES_PER_SOUND_UNIT];
			struct NativeAudioPcmBuffer tmp;
			int i;

			memset(&tmp, 0, sizeof(tmp));
			tmp.samples = tmpSamples;
			tmp.capacity = XA_SAMPLES_PER_SOUND_UNIT;
			if (!NativeAudio_DecodeXA28Nibbles(sector, frameOff, block, 0, 0, state, &tmp) || (tmp.count < XA_SAMPLES_PER_SOUND_UNIT))
			{
				return 0;
			}
			for (i = 0; i < XA_SAMPLES_PER_SOUND_UNIT; i++)
			{
				left[i] = tmp.samples[i];
			}
			tmp.count = 0;

			if (!NativeAudio_DecodeXA28Nibbles(sector, frameOff, block, 1, 1, state, &tmp) || (tmp.count < XA_SAMPLES_PER_SOUND_UNIT))
			{
				return 0;
			}
			for (i = 0; i < XA_SAMPLES_PER_SOUND_UNIT; i++)
			{
				right[i] = tmp.samples[i];
			}

			for (i = 0; i < XA_SAMPLES_PER_SOUND_UNIT; i++)
			{
				if (!NativeAudio_PcmPush(out, left[i]) || !NativeAudio_PcmPush(out, right[i]))
				{
					return 0;
				}
			}
		}
	}

	return 1;
}

internal void NativeAudio_XAStreamInitRingBackingIfNeeded(void)
{
	if (s_audio.xaStream.ringPcm == NULL)
	{
		s_audio.xaStream.ringPcm = s_xaRingPcmLive;
	}
	if (s_audio.xaPendingStream.ringPcm == NULL)
	{
		s_audio.xaPendingStream.ringPcm = s_xaRingPcmPending;
	}
#if defined(__3DS__)
	if (s_audio.xaStream.spanBuf == NULL)
	{
		s_audio.xaStream.spanBuf = s_xaSpanBufLive;
	}
	if (s_audio.xaPendingStream.spanBuf == NULL)
	{
		s_audio.xaPendingStream.spanBuf = s_xaSpanBufPending;
	}
#endif
	/* Both platforms: the span cache is 3DS-only but the read staging and its
	 * role tag are not. */
	if (s_audio.xaStream.discScratch == NULL)
	{
		s_xaDiscScratchLive.owner = NATIVE_DISC_IMAGE_OWNER_AUDIO;
		s_audio.xaStream.discScratch = &s_xaDiscScratchLive;
	}
	if (s_audio.xaPendingStream.discScratch == NULL)
	{
		s_xaDiscScratchPending.owner = NATIVE_DISC_IMAGE_OWNER_MAIN;
		s_audio.xaPendingStream.discScratch = &s_xaDiscScratchPending;
	}
}

// Preserves the assigned ring backing array across the clear -- ringPcm is
// assigned once, lazily (see above), and never freed or reassigned again for
// the rest of the process lifetime.
internal void NativeAudio_XAStreamClearNoLock(struct NativeAudioXaStream *stream)
{
	s16 *ringPcm = stream->ringPcm;
	u8 *spanBuf = stream->spanBuf;
	struct NativeDiscImageScratch *discScratch = stream->discScratch;

	memset(stream, 0, sizeof(*stream));
	stream->ringPcm = ringPcm;
	stream->spanBuf = spanBuf; /* spanSectorCount just went to 0 = span invalidated */
	stream->discScratch = discScratch;
}

internal void NativeAudio_XAStreamSwap(struct NativeAudioXaStream *a, struct NativeAudioXaStream *b)
{
	/*
	 * Swap everything EXCEPT the per-stream decode scratch (sectorBuf/
	 * tempFrames): scratch contents are transient within a single
	 * NativeAudio_XAStreamDecodeNextSector call and never need to cross a
	 * swap. Doing it this way is not an optimization -- a whole-struct
	 * temporary here is a ~37KB stack local now that the scratch lives in
	 * the struct, and it smashed the thread stack at the first
	 * pending->live track promote.
	 */
	unsigned char tmp[offsetof(struct NativeAudioXaStream, sectorBuf)];

	memcpy(tmp, a, sizeof(tmp));
	memcpy(a, b, sizeof(tmp));
	memcpy(b, tmp, sizeof(tmp));
}

internal int NativeAudio_XASourceOpen(struct NativeAudioXaSource *source, const char *path, int channelFilter)
{
	struct NativeAssetsHostRange hostFile;
	struct NativeDiscImageFile discFile;
	int sectorSize;
	int sectorBase;
	int totalSectors;

	memset(source, 0, sizeof(*source));

	if ((channelFilter < 0) || (channelFilter > 0xff))
	{
		return 0;
	}

	// Host-file-first, then disc-image fallback -- mirrors NativeCD_OpenFile's
	// idiom (platform/native_cd.c) and NativeAssets_ReadBytes's precedence
	// (platform/native_assets.c:722-733).
	if (NativeAssets_OpenHostRange(path, &hostFile))
	{
		if ((hostFile.size <= 0) || (hostFile.size > INT_MAX) ||
		    !NativeAudio_GetXASectorLayout((int)hostFile.size, &sectorSize, &sectorBase, &totalSectors))
		{
			NativeAssets_CloseHostRange(&hostFile);
			return 0;
		}

		source->useDiscImage = 0;
		source->hostFile = hostFile;
		source->sectorSize = sectorSize;
		source->sectorBase = sectorBase;
		source->totalSectors = totalSectors;
		source->channelFilter = channelFilter;
		return 1;
	}

	if (!NativeDiscImage_FindFile(path, &discFile) || (discFile.size == 0))
	{
		return 0;
	}

	// NativeDiscImage_ReadRawSectors always normalizes to
	// NATIVE_DISC_IMAGE_MODE2_USER_SIZE (2336) bytes per sector, so hardcode
	// the disc-image layout instead of autodetecting via
	// NativeAudio_GetXASectorLayout (that autodetection is for raw host-file
	// byte layouts, which can genuinely be either 2352 or 2336).
	//
	// The sector COUNT must come from the disc layer's own rule, not from
	// size/2336: the ISO directory records these XA extents at 2048 bytes per
	// sector (S00.XA is exactly 10416*2048), so dividing by 2336 sees only the
	// first ~88% of the file and clips every track whose slot lies in the tail
	// (the victory/defeat music loses its last ~8 seconds).
	source->useDiscImage = 1;
	source->discFile = discFile;
	source->sectorSize = XA_FORM2_SECTOR_SIZE;
	source->sectorBase = 0;
	source->totalSectors = (int)NativeDiscImage_RawSectorCount(discFile.size);
	source->channelFilter = channelFilter;
	return source->totalSectors > 0;
}

#if defined(__3DS__)
/* A PAL voice pack file: an absolute host path (not under the assets dir), raw 2352-byte sectors. */
internal int NativeAudio_XASourceOpenPal(struct NativeAudioXaSource *source, const char *path, int channelFilter)
{
	FILE *file;
	long size;
	int sectorSize;
	int sectorBase;
	int totalSectors;

	memset(source, 0, sizeof(*source));

	if ((channelFilter < 0) || (channelFilter > 0xff) || (path == NULL))
	{
		return 0;
	}

	file = fopen(path, "rb");
	if (file == NULL)
	{
		return 0;
	}

	if ((fseek(file, 0, SEEK_END) != 0) || ((size = ftell(file)) <= 0) || (size > INT_MAX) || (fseek(file, 0, SEEK_SET) != 0) ||
	    !NativeAudio_GetXASectorLayout((int)size, &sectorSize, &sectorBase, &totalSectors))
	{
		fclose(file);
		return 0;
	}

	source->useDiscImage = 0;
	source->hostFile.file = file;
	source->hostFile.size = size;
	source->sectorSize = sectorSize;
	source->sectorBase = sectorBase;
	source->totalSectors = totalSectors;
	source->channelFilter = channelFilter;
	return 1;
}
#endif

/* Path and open for a track of `pack` (0 = the disc's own files). Identical to the two plain calls when pack is 0. */
internal int NativeAudio_BuildXAPathForPack(char *path, size_t pathSize, int pack, int categoryID, int fileNumber)
{
#if defined(__3DS__)
	if (pack != 0)
	{
		return NativeAudio_PalVoicePath(path, pathSize, pack, categoryID, fileNumber);
	}
#else
	(void)pack;
#endif
	return NativeAudio_BuildXAPath(path, pathSize, categoryID, fileNumber);
}

internal int NativeAudio_XASourceOpenForPack(struct NativeAudioXaSource *source, const char *path, int pack, int channelFilter)
{
#if defined(__3DS__)
	if (pack != 0)
	{
		return NativeAudio_XASourceOpenPal(source, path, channelFilter);
	}
#else
	(void)pack;
#endif
	return NativeAudio_XASourceOpen(source, path, channelFilter);
}

/*
 * `scratch` names the disc-read staging this call may use, i.e. which thread
 * is asking (see struct NativeDiscImageScratch). NULL means "main thread,
 * use the shared main scratch" -- correct for the open/count path and for
 * every PC caller, since PC pumps XA from the vblank caller. The 3DS worker
 * must always pass its stream's own scratch.
 */
internal int NativeAudio_XASourceReadSector(struct NativeAudioXaSource *source, int sectorIndex, u8 *dst, struct NativeDiscImageScratch *scratch)
{
	if ((sectorIndex < 0) || (sectorIndex >= source->totalSectors))
	{
		return 0;
	}

#if defined(__3DS__)
	if (source->ramSectors != NULL)
	{
		/* Above the [DISCIO] counter on purpose: a RAM hit is not an SD call,
		 * and the whole point of the voice cache is that this line reads 0
		 * mid-race. */
		memcpy(dst, source->ramSectors + (size_t)sectorIndex * (size_t)source->sectorSize, (size_t)source->sectorSize);
		return 1;
	}
#endif

	if (source->useDiscImage)
	{
#if CTR_NATIVE_DISC_IO_PROFILE
		/* Main-bucket singles only: the worker's fallback reads land in the
		 * audio [DISCIO] line, where runs1 already names them. Owner-MAIN
		 * scratches are main-thread-only by contract, so this counter keeps
		 * the one-writer rule the site tallies rely on. */
		if ((scratch == NULL) || (scratch->owner == NATIVE_DISC_IMAGE_OWNER_MAIN))
		{
			s_nativeDiscIoSiteXaOne++;
		}
#endif
		if (scratch != NULL)
		{
			return NativeDiscImage_ReadRawSectorsScratch(&source->discFile, (u32)sectorIndex, 1, dst, scratch);
		}

		return NativeDiscImage_ReadRawSectors(&source->discFile, (u32)sectorIndex, 1, dst);
	}

	return NativeAssets_ReadHostRange(&source->hostFile, (long)sectorIndex * source->sectorSize, dst, (size_t)source->sectorSize);
}

/*
 * One sequential read of `count` physical sectors -- both backends already
 * had a bulk primitive, the per-sector path just never used it. Layout in
 * dst matches the per-sector reads exactly: contiguous sectors at
 * source->sectorSize stride (the disc backend normalizes to 2336/sector,
 * the host backend is whatever the file's raw stride is).
 *
 * Not 3DS-gated: the span cache above it is, but the open-time header scan
 * (NativeAudio_XAStreamCountAudioSectors) wants this on both platforms.
 */
internal int NativeAudio_XASourceReadSectors(struct NativeAudioXaSource *source, int firstSector, int count, u8 *dst, struct NativeDiscImageScratch *scratch)
{
	if ((firstSector < 0) || (count <= 0) || (firstSector > source->totalSectors - count))
	{
		return 0;
	}

#if defined(__3DS__)
	if (source->ramSectors != NULL)
	{
		memcpy(dst, source->ramSectors + (size_t)firstSector * (size_t)source->sectorSize, (size_t)count * (size_t)source->sectorSize);
		return 1;
	}
#endif

	if (source->useDiscImage)
	{
		return NativeDiscImage_ReadRawSectorsScratch(&source->discFile, (u32)firstSector, (u32)count, dst, scratch);
	}

	return NativeAssets_ReadHostRange(&source->hostFile, (long)firstSector * source->sectorSize, dst, (size_t)((long)count * source->sectorSize));
}

#if defined(__3DS__)
/*
 * Read one physical sector through the stream's span cache: a miss fetches
 * NATIVE_AUDIO_XA_SPAN_SECTORS sectors ahead in one bulk call, then this and
 * the following ~31 requests are memcpys. "Bulk" is now literal -- the call
 * costs ceil(32 / NATIVE_DISC_IMAGE_CHUNK_SECTORS) SD reads. It was not:
 * until the disc reader learned to read a run, this same call was a loop of
 * 32 single-sector seek+read pairs, so the span cut FAT-mutex episodes per
 * decoded sector without cutting SD round trips at all. The decode loop asks
 * for physical sectors strictly in ascending order (nextPhysicalSector++), so one span
 * fill amortizes across everything it covers -- including the interleaved
 * other-channel sectors the caller will discard, which under per-sector
 * reads each cost their own trip through the FAT mutex.
 *
 * Threading: per-stream state only, same disjointness contract as the
 * per-stream decode scratch (worker owns the live stream, main thread owns
 * pending) -- see the sectorBuf comment on struct NativeAudioXaStream.
 * Falls back to the single-sector path if the span cannot be filled (tail
 * of file shorter than a span is handled by shrinking the fill; a failed
 * bulk read falls through so the existing error handling stays in charge).
 */
/* Span fills since the last NativeAudio_TakeXaSpanProfile: how many, how many
 * physical sectors they asked for, and the time inside the read. Written by
 * whichever one thread pumps the live stream; the pending stream's fills on
 * the game thread can race a worker's by a count, which a diagnostic allows. */
internal unsigned s_xaSpanProfFills;
internal unsigned s_xaSpanProfSectors;
internal unsigned long long s_xaSpanProfTicks;

void NativeAudio_TakeXaSpanProfile(unsigned *fills, unsigned *sectors, unsigned long long *ticks)
{
	*fills = s_xaSpanProfFills;
	*sectors = s_xaSpanProfSectors;
	*ticks = s_xaSpanProfTicks;
	s_xaSpanProfFills = 0;
	s_xaSpanProfSectors = 0;
	s_xaSpanProfTicks = 0;
}

/*
 * Game-thread span ring (no audio worker): spanBuf holds the physical sectors
 * [spanStartSector, spanStartSector + spanSectorCount), sector s in slot
 * s % NATIVE_AUDIO_XA_SPAN_SECTORS. The decode reads sequentially from
 * nextPhysicalSector, so each append first drops what the cursor has passed.
 * Reads at most maxSectors; returns the count read, or -1 on a read failure
 * (sectors read before it stay in the ring).
 */
internal int NativeAudio_XASpanRingAppend(struct NativeAudioXaStream *stream, int maxSectors)
{
	struct NativeAudioXaSource *source = &stream->source;
	int cursor = stream->nextPhysicalSector;
	int end = stream->spanStartSector + stream->spanSectorCount;
	int read = 0;

	if ((cursor < stream->spanStartSector) || (cursor >= end))
	{
		stream->spanStartSector = cursor;
		stream->spanSectorCount = 0;
	}
	else
	{
		stream->spanSectorCount = end - cursor;
		stream->spanStartSector = cursor;
	}

	while (read < maxSectors)
	{
		int chunk = maxSectors - read;
		int slot;
		unsigned long long fillStart;
		int ok;

		end = stream->spanStartSector + stream->spanSectorCount;
		slot = end % NATIVE_AUDIO_XA_SPAN_SECTORS;
		if (chunk > NATIVE_AUDIO_XA_SPAN_SECTORS - stream->spanSectorCount)
		{
			chunk = NATIVE_AUDIO_XA_SPAN_SECTORS - stream->spanSectorCount;
		}
		if (chunk > NATIVE_AUDIO_XA_SPAN_SECTORS - slot)
		{
			chunk = NATIVE_AUDIO_XA_SPAN_SECTORS - slot;
		}
		if (chunk > source->totalSectors - end)
		{
			chunk = source->totalSectors - end;
		}
		if (chunk <= 0)
		{
			break;
		}

		fillStart = N3DS_AudioTicksNow();
		ok = NativeAudio_XASourceReadSectors(source, end, chunk, stream->spanBuf + (size_t)slot * (size_t)source->sectorSize, stream->discScratch);
		s_xaSpanProfTicks += N3DS_AudioTicksNow() - fillStart;
		s_xaSpanProfFills++;
		s_xaSpanProfSectors += (unsigned)chunk;
		if (!ok)
		{
			return -1;
		}
		stream->spanSectorCount += chunk;
		read += chunk;
	}

	return read;
}

/* Returns 1 with the sector in dst, 0 on a read failure, or -1 when mayBlock
 * is 0 and the sector is not read yet (game-thread ring only). */
internal int NativeAudio_XAStreamReadSectorBuffered(struct NativeAudioXaStream *stream, int sectorIndex, u8 *dst, int mayBlock)
{
	struct NativeAudioXaSource *source = &stream->source;

	if (source->ramSectors != NULL)
	{
		/* RAM-backed voice-cache source: the span cache would only add a
		 * second memcpy. Straight through to the RAM branch. */
		return NativeAudio_XASourceReadSector(source, sectorIndex, dst, stream->discScratch);
	}

	if ((stream->spanBuf == NULL) || (stream->discScratch == NULL) || (source->sectorSize <= 0) || (source->sectorSize > XA_FULL_SECTOR_SIZE))
	{
		return NativeAudio_XASourceReadSector(source, sectorIndex, dst, stream->discScratch);
	}

	if (!N3DS_AudioThreadIsRunning())
	{
		if (sectorIndex != stream->nextPhysicalSector)
		{
			/* The ring follows the decode cursor; anything else reads direct. */
			return NativeAudio_XASourceReadSector(source, sectorIndex, dst, stream->discScratch);
		}
		if ((sectorIndex < stream->spanStartSector) || (sectorIndex >= stream->spanStartSector + stream->spanSectorCount))
		{
			if (!mayBlock)
			{
				return -1;
			}
			if ((NativeAudio_XASpanRingAppend(stream, NATIVE_AUDIO_XA_SPAN_SECTORS_INLINE) <= 0) || (sectorIndex < stream->spanStartSector) ||
			    (sectorIndex >= stream->spanStartSector + stream->spanSectorCount))
			{
				stream->spanSectorCount = 0;
				return NativeAudio_XASourceReadSector(source, sectorIndex, dst, stream->discScratch);
			}
		}
		memcpy(dst, stream->spanBuf + (size_t)(sectorIndex % NATIVE_AUDIO_XA_SPAN_SECTORS) * (size_t)source->sectorSize, (size_t)source->sectorSize);
		return 1;
	}

	if ((sectorIndex < stream->spanStartSector) || (sectorIndex >= stream->spanStartSector + stream->spanSectorCount))
	{
		int fill = N3DS_AudioThreadIsRunning() ? NATIVE_AUDIO_XA_SPAN_SECTORS : NATIVE_AUDIO_XA_SPAN_SECTORS_INLINE;
		unsigned long long fillStart;
		int filled;

		if (fill > source->totalSectors - sectorIndex)
		{
			fill = source->totalSectors - sectorIndex;
		}
		fillStart = N3DS_AudioTicksNow();
		filled = (fill > 0) && NativeAudio_XASourceReadSectors(source, sectorIndex, fill, stream->spanBuf, stream->discScratch);
		s_xaSpanProfTicks += N3DS_AudioTicksNow() - fillStart;
		s_xaSpanProfFills++;
		s_xaSpanProfSectors += (fill > 0) ? (unsigned)fill : 0u;
		if (!filled)
		{
			stream->spanSectorCount = 0;
			return NativeAudio_XASourceReadSector(source, sectorIndex, dst, stream->discScratch);
		}
		stream->spanStartSector = sectorIndex;
		stream->spanSectorCount = fill;
	}

	memcpy(dst, stream->spanBuf + (size_t)(sectorIndex - stream->spanStartSector) * (size_t)source->sectorSize, (size_t)source->sectorSize);
	return 1;
}
#endif

internal void NativeAudio_XASourceClose(struct NativeAudioXaSource *source)
{
#if defined(__3DS__)
	if (source->ramSectors != NULL)
	{
		/* Nothing to close: the blob outlives every stream. */
		memset(source, 0, sizeof(*source));
		return;
	}
#endif
	if (!source->useDiscImage)
	{
		NativeAssets_CloseHostRange(&source->hostFile);
	}
	memset(source, 0, sizeof(*source));
}

// Header-only pass (no ADPCM decode, no whole-file malloc) counting sectors
// that match this source's channelFilter. Needed only for the arbitrary-path
// API (NativeAudio_XAStreamOpenFile): unlike the category-based track path,
// which trusts the XNF metadata's declared sector count exactly like the old
// LoadXATrackPcm did, an arbitrary file can multiplex XA audio with other
// content (e.g. TEST.STR's interleaved MDEC video -- see MM_Scrapbook.c, the
// one real caller of NativeAudio_PlayXAFile), so the physical sector count is
// not a safe proxy for decodable frame count.
internal int NativeAudio_XAStreamCountAudioSectors(struct NativeAudioXaSource *source)
{
	/* Chunked, not per-sector. This scan walks the WHOLE file to look at one
	 * header per sector, and it used to spend one seek+read on each -- 2214
	 * of them for TEST.STR on a boot-to-attract run, measured, which made it
	 * the single largest source of disc reads in the process by a wide
	 * margin. It runs on the main thread at track-open time, which is the
	 * shape of the "race-start jingle takes forever" complaint. Reads are
	 * strictly sequential, so a chunk buffer costs one read per 16 sectors
	 * and changes nothing about what the scan sees. */
	local_persist u8 s_countChunkBuf[NATIVE_DISC_IMAGE_CHUNK_SECTORS * XA_FULL_SECTOR_SIZE];
	int count = 0;
	int sector = 0;

	if ((source->sectorSize <= 0) || (source->sectorSize > XA_FULL_SECTOR_SIZE))
	{
		return 0;
	}

	while (sector < source->totalSectors)
	{
		int chunk = source->totalSectors - sector;
		int i;

		if (chunk > NATIVE_DISC_IMAGE_CHUNK_SECTORS)
		{
			chunk = NATIVE_DISC_IMAGE_CHUNK_SECTORS;
		}

#if CTR_NATIVE_DISC_IO_PROFILE
		s_nativeDiscIoSiteXaCount++;
#endif
		if (!NativeAudio_XASourceReadSectors(source, sector, chunk, s_countChunkBuf, &s_xaDiscScratchPending))
		{
			break;
		}

		for (i = 0; i < chunk; i++)
		{
			if (NativeAudio_IsXAAudioSector(&s_countChunkBuf[(size_t)i * (size_t)source->sectorSize], source->sectorBase, source->channelFilter))
			{
				count++;
			}
		}

		sector += chunk;
	}

	return count;
}

#if defined(__3DS__)
/*
 * XA voice cache (the audio preloader). Mid-race character voice
 * lines, announcer calls, and the wrong-way barks are CD-XA streams opened
 * per PLAY, and every open cost two XNF reads, a whole-file channel scan, and
 * a prime read -- ~26ms fixed per SD call, synchronously on the main thread
 * (the [DISCIO] hardware verdict; ADPCM decode was innocent). SFX and engine
 * audio never touch the disc mid-race (SPU-resident banks with a silent no-op
 * guard), so this cache IS the whole preloader: at the level loading screen,
 * read every GAME-category track plus the two EXTRA wrong-way barks into RAM
 * once per boot (~11.6 MiB -- measured off the retail disc: GAME is 5004
 * matching sectors across 21 files) and serve every later open AND the whole
 * pump lifetime from memory. Tracks are stored as verbatim physical sectors
 * (subheader included, source stride), so the decode path is untouched: a
 * RAM source is just a source whose every physical sector happens to match
 * the channel filter.
 *
 * MUSIC stays on the SD path: race music is CSEQ (sequenced SPU, not XA), the
 * XA MUSIC files are 26 MiB, and the count memo below still removes their
 * repeat whole-file scans after the first open of each track. The exception
 * is a console with no audio thread, where the stings that play mid-race are
 * cached too (s_xaCacheRaceStings).
 */
/* Blob size, fixed before the blob is allocated. A New 3DS has the heap for
 * every GAME and EXTRA line. An original 3DS in its standard 64 MiB mode (every
 * .3dsx) has about 14 MiB of heap for the whole game, about 3.7 MiB of which the
 * rest of the game needs at its peak in a race, so it caches up to 8 MiB (the
 * race stings first, then voice lines) while leaving
 * NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_RESERVE free, and the rest stream from the
 * SD card. An original 3DS running the CIA in an extended memory mode has a heap
 * of at least NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_EXT_HEAP and caches up to the
 * New 3DS size while leaving NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_EXT_RESERVE free. A build with less heap (a larger executable) gets
 * a smaller cache, not a level that fails to load. */
#define NATIVE_AUDIO_XA_VOICE_CACHE_NEW3DS_BYTES  (32u * 1024u * 1024u)
#define NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_BYTES  (8u * 1024u * 1024u)
#define NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_EXT_HEAP (24u * 1024u * 1024u)
/* Left free in extended memory: the game's own heap peaks near 5.4 MiB in a race on the Old 3DS. */
#define NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_EXT_RESERVE (8u * 1024u * 1024u)
#define NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_RESERVE (5u * 1024u * 1024u)
#define NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_STEP   (512u * 1024u)
internal u32 s_xaVoiceCacheMaxBytes = NATIVE_AUDIO_XA_VOICE_CACHE_NEW3DS_BYTES;
#define NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES (s_xaVoiceCacheMaxBytes)
#define NATIVE_AUDIO_XA_TRACK_INDEX_MAX       512
/* Cache/memo keys: the disc's tracks use their XNF entryIndex, a PAL pack's are offset by pack * 512. */
#define NATIVE_AUDIO_XA_KEY_MAX               ((NATIVE_AUDIO_PAL_PACK_COUNT + 1) * NATIVE_AUDIO_XA_TRACK_INDEX_MAX)
_Static_assert(NATIVE_AUDIO_PAL_KEY_STRIDE == NATIVE_AUDIO_XA_TRACK_INDEX_MAX, "PAL voice keys must stride by the disc's key space");
#define NATIVE_AUDIO_XA_CATEGORY_MUSIC        0
#define NATIVE_AUDIO_XA_CATEGORY_EXTRA        1
#define NATIVE_AUDIO_XA_CATEGORY_GAME         2
/* All of EXTRA is cached. 0..0x3d is the Aku/Uka speech table (hint IDs
 * 0..0x1d in Aku's voice, the same rows at +ADV_MASK_HINT_UKA_UKA_XA_OFFSET
 * in Uka's, the wrong-way barks as the last row of each half) -- hints fire
 * while DRIVING the adventure hubs and the barks mid-race, 13.0 MiB
 * measured. The rest (narration, N.Tropy lines, 4.8 MiB) rode along after
 * hardware showed the intro narration popping against boot's SD contention
 * (4.2s of reads, worst call 41ms on the slow card) -- owner asked for it
 * cached. The one EXTRA play that still streams is MainMain's boot-time
 * XAPlay(EXTRA, 0x50): it fires before the first loading screen, so the
 * cache does not exist yet. */

struct NativeAudioXaVoiceCacheTrack
{
	u32 byteOffset;  // into the blob
	s32 sectorCount; // 0 = not cached
	s16 sectorSize;  // stored stride = the build-time source's stride
	s16 sectorBase;  // subheader offset within a stored sector
};

struct NativeAudioXaVoiceCache
{
	b32 built; // build ran (only then is a lookup miss an alarm worth logging)
	u8 *blob;
	u32 blobUsed;
	struct NativeAudioXaVoiceCacheTrack tracks[NATIVE_AUDIO_XA_KEY_MAX]; // by track key (XNF entryIndex, PAL packs offset)
};

internal struct NativeAudioXaVoiceCache s_xaVoiceCache;

/* Audio-sector counts by XNF entryIndex, 0 = unknown. Filled by the cache
 * build and memoized from every full CountAudioSectors run, so an uncached
 * track (MUSIC, cutscene EXTRA) pays its whole-file scan once per boot, not
 * once per play. */
internal s32 s_xaAudioSectorCountMemo[NATIVE_AUDIO_XA_KEY_MAX];

/* Which voice pack the RAM cache holds (0 = the disc's English): the effective pack of the language
 * selected when it was built, never a pack that is not installed, because the build keeps only lines
 * whose lookup lands in this pack. A different voice language is cached by
 * NativeAudio_VoiceCacheRebuildBegin; until then its lines stream from the SD card. */
internal int s_xaCachePack;

/* Build scratch: wanted tracks of the category being built. Static because
 * 512 entries is stack-hostile on the 3DS main thread. Only touched by the
 * cache build (main thread). */
internal struct
{
	s32 xaID;
	struct NativeAudioXaTrackInfo info;
} s_xaVoiceCacheScratch[NATIVE_AUDIO_XA_TRACK_INDEX_MAX];

/* To sdmc:/ctr3ds.log via the backend logger -- Platform_Log/stdout are
 * invisible there (stdout is the bottom-screen console, and an unpositioned
 * line scrolls the stats block; see NativeAudio_GetOutputStats). */
__attribute__((format(printf, 1, 2))) internal void NativeAudio_XAVoiceCacheLog(const char *fmt, ...)
{
	char line[160];
	va_list args;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	N3DS_DebugLog(line);
}

/* The MUSIC tracks that play during a race: the start-of-race music (xa 0-3,
 * 146 sectors each) and the final-lap sting (xa 6, 31 sectors), 1.4 MiB
 * together. Cached only where there is no audio thread, so their SD reads stay
 * off the game thread; the finish music (xa 4 and 5, 3 MiB a track) streams. */
internal b32 s_xaCacheRaceStings;

internal int NativeAudio_XAVoiceCacheWantsTrack(int categoryID, int xaID)
{
	if (categoryID == NATIVE_AUDIO_XA_CATEGORY_GAME)
	{
		return 1;
	}
	if (categoryID == NATIVE_AUDIO_XA_CATEGORY_MUSIC)
	{
		return s_xaCacheRaceStings && (((xaID >= 0) && (xaID <= 3)) || (xaID == 6));
	}
	return categoryID == NATIVE_AUDIO_XA_CATEGORY_EXTRA;
}

/* The pack a category's cached tracks come from: music is always the disc's. */
internal int NativeAudio_XAVoiceCachePackFor(int categoryID)
{
	return (categoryID == NATIVE_AUDIO_XA_CATEGORY_MUSIC) ? 0 : s_xaCachePack;
}

/* Open `source` RAM-backed from the cache. Caller must have cleared the
 * stream already (the source is rebuilt from scratch here). */
internal int NativeAudio_XAVoiceCacheOpen(struct NativeAudioXaSource *source, const struct NativeAudioXaTrackInfo *info)
{
	const struct NativeAudioXaVoiceCacheTrack *track;

	if (!s_xaVoiceCache.built || (s_xaVoiceCache.blob == NULL) || (info->entryIndex < 0) || (info->entryIndex >= NATIVE_AUDIO_XA_KEY_MAX))
	{
		return 0;
	}

	track = &s_xaVoiceCache.tracks[info->entryIndex];
	if (track->sectorCount <= 0)
	{
		return 0;
	}

	memset(source, 0, sizeof(*source));
	source->ramSectors = s_xaVoiceCache.blob + track->byteOffset;
	source->sectorSize = track->sectorSize;
	source->sectorBase = track->sectorBase;
	source->totalSectors = track->sectorCount;
	source->channelFilter = info->channelFilter;
	return 1;
}

/* Aggregate progress for the whole boot XA warm (music memo + both voice-cache
 * categories); see NativeAudio_VoiceCacheEnsureBegin/Step. */
internal struct
{
	b32 active;
	int stage; /* 0=music warm, 4=cache the race stings (no audio thread only), 1=cache GAME, 2=cache EXTRA, 3=finalize */
	int tracks;
	int sectors;
	u32 readSectors;    /* disc/SD sectors this warm has actually read */
	u32 plannedSectors; /* what it set out to read; 0 until the plan is known */
} s_vce;

/* Resumable state for one category's build pass (GAME then EXTRA). A step
 * reads at most one bounded chunk of one file's span per call -- per-CALL SD
 * cost is the whole reason this runs spread across the loading screen instead
 * of blocking -- and only de-interleaves a file's wanted tracks into the blob
 * once that file's whole span has landed (CPU-only work, not chunked). */
internal struct
{
	b32 active;
	b32 srcOpen;
	int categoryID;
	int pack; /* voice pack being cached (s_xaCachePack) */
	int numWanted;
	u32 filesWanted;
	int fileNumber;
	struct NativeAudioXaSource src;
	u8 *fileBuf;
	int span;
	int pos;
} s_xacb;

/* Disc sectors one category's build pass sets out to read: each wanted file is
 * read once, across the longest slot any wanted track occupies in it -- the
 * same rule NativeAudio_XAVoiceCacheBuildCategoryStep applies at open time,
 * minus the clamp to the file's real length, which is not known until the open.
 * Slot sizes come from the XNF, so this is a plan, not an outcome. */
internal u32 NativeAudio_XAVoiceCachePlannedSectors(int categoryID)
{
	int fileMax[32];
	u32 total = 0;
	int xaID;
	int f;

	memset(fileMax, 0, sizeof(fileMax));

	for (xaID = 0; xaID < NATIVE_AUDIO_XA_TRACK_INDEX_MAX; xaID++)
	{
		struct NativeAudioXaTrackInfo info;

		if (!NativeAudio_XAVoiceCacheWantsTrack(categoryID, xaID) || !NativeAudio_LookupXATrackInfo(categoryID, xaID, &info))
		{
			continue;
		}
		if ((info.fileNumber < 0) || (info.fileNumber >= 32) || (info.entryIndex >= NATIVE_AUDIO_XA_KEY_MAX) ||
		    (info.pack != NativeAudio_XAVoiceCachePackFor(categoryID)))
		{
			continue;
		}
		if (info.numSectors > fileMax[info.fileNumber])
		{
			fileMax[info.fileNumber] = info.numSectors;
		}
	}

	for (f = 0; f < 32; f++)
	{
		total += (u32)fileMax[f];
	}
	return total;
}

/* The largest blob up to the original 3DS cap for this heap that leaves the
 * reserve free, found by allocating blob and reserve together (the heap's free
 * space is not visible from this file). 0 when not even one step fits. */
internal u32 NativeAudio_XAVoiceCacheOld3DSBytes(void)
{
	const int extended = N3DS_HeapSizeBytes() >= NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_EXT_HEAP;
	const u32 cap = extended ? NATIVE_AUDIO_XA_VOICE_CACHE_NEW3DS_BYTES : NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_BYTES;
	const u32 reserve = extended ? NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_EXT_RESERVE : NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_RESERVE;
	u32 bytes;

	for (bytes = cap; bytes >= NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_STEP; bytes -= NATIVE_AUDIO_XA_VOICE_CACHE_OLD3DS_STEP)
	{
		/* volatile: a malloc whose result is only freed is removable, and GCC then treats it as succeeding. */
		void *volatile probe = malloc((size_t)bytes + reserve);

		if (probe != NULL)
		{
			free(probe);
			return bytes;
		}
	}
	return 0;
}

internal void NativeAudio_XAVoiceCacheBuildCategoryBegin(int categoryID)
{
	int xaID;

	memset(&s_xacb, 0, sizeof(s_xacb));
	s_xacb.categoryID = categoryID;
	s_xacb.pack = NativeAudio_XAVoiceCachePackFor(categoryID);

	for (xaID = 0; xaID < NATIVE_AUDIO_XA_TRACK_INDEX_MAX; xaID++)
	{
		struct NativeAudioXaTrackInfo info;

		if (!NativeAudio_XAVoiceCacheWantsTrack(categoryID, xaID) || !NativeAudio_LookupXATrackInfo(categoryID, xaID, &info))
		{
			continue;
		}
		if ((info.fileNumber < 0) || (info.fileNumber >= 32) || (info.entryIndex >= NATIVE_AUDIO_XA_KEY_MAX) || (info.pack != s_xacb.pack))
		{
			continue;
		}
		s_xaVoiceCacheScratch[s_xacb.numWanted].xaID = xaID;
		s_xaVoiceCacheScratch[s_xacb.numWanted].info = info;
		s_xacb.numWanted++;
		s_xacb.filesWanted |= 1u << info.fileNumber;
	}
	s_xacb.active = 1;
}

/* Advances the category build by at most `budgetSectors` sectors of disc/SD
 * I/O (0 = uncapped, still bounded by NATIVE_DISC_IMAGE_CHUNK_SECTORS per
 * call). Returns nonzero once this category has no more files to process. */
internal int NativeAudio_XAVoiceCacheBuildCategoryStep(int budgetSectors)
{
	if (!s_xacb.active)
	{
		return 1;
	}

	while (!s_xacb.srcOpen)
	{
		char path[256];
		int i;

		if (s_xacb.fileNumber >= 32)
		{
			s_xacb.active = 0;
			return 1;
		}
		if ((s_xacb.filesWanted & (1u << s_xacb.fileNumber)) == 0)
		{
			s_xacb.fileNumber++;
			continue;
		}
		if (!NativeAudio_BuildXAPathForPack(path, sizeof(path), s_xacb.pack, s_xacb.categoryID, s_xacb.fileNumber) ||
		    !NativeAudio_XASourceOpenForPack(&s_xacb.src, path, s_xacb.pack, 0))
		{
			NativeAudio_XAVoiceCacheLog("[XAPRELOAD] cannot open %s -- its tracks stay on the SD path", path);
			s_xacb.fileNumber++;
			continue;
		}

		s_xacb.span = 0;
		for (i = 0; i < s_xacb.numWanted; i++)
		{
			if (s_xaVoiceCacheScratch[i].info.fileNumber == s_xacb.fileNumber)
			{
				int trackSpan = s_xaVoiceCacheScratch[i].info.numSectors;

				if (trackSpan > s_xacb.src.totalSectors)
				{
					trackSpan = s_xacb.src.totalSectors;
				}
				if (trackSpan > s_xacb.span)
				{
					s_xacb.span = trackSpan;
				}
			}
		}

		s_xacb.fileBuf = (s_xacb.span > 0) ? (u8 *)malloc((size_t)s_xacb.span * (size_t)s_xacb.src.sectorSize) : NULL;
		if (s_xacb.fileBuf == NULL)
		{
			NativeAudio_XAVoiceCacheLog("[XAPRELOAD] no %d KiB scratch for %s -- its tracks stay on the SD path",
			                            (s_xacb.span * s_xacb.src.sectorSize) >> 10, path);
			NativeAudio_XASourceClose(&s_xacb.src);
			s_xacb.fileNumber++;
			continue;
		}

		s_xacb.pos = 0;
		s_xacb.srcOpen = 1;
	}

	if (s_xacb.pos < s_xacb.span)
	{
		int chunk = s_xacb.span - s_xacb.pos;

		if (chunk > NATIVE_DISC_IMAGE_CHUNK_SECTORS)
		{
			chunk = NATIVE_DISC_IMAGE_CHUNK_SECTORS;
		}
		if ((budgetSectors > 0) && (chunk > budgetSectors))
		{
			chunk = budgetSectors;
		}

		if (!NativeAudio_XASourceReadSectors(&s_xacb.src, s_xacb.pos, chunk, s_xacb.fileBuf + (size_t)s_xacb.pos * (size_t)s_xacb.src.sectorSize,
		                                     &s_xaDiscScratchPending))
		{
			NativeAudio_XAVoiceCacheLog("[XAPRELOAD] short read on file %d cat=%d (%d of %d sectors) -- its tracks stay on the SD path",
			                            s_xacb.fileNumber, s_xacb.categoryID, s_xacb.pos, s_xacb.span);
			free(s_xacb.fileBuf);
			s_xacb.fileBuf = NULL;
			NativeAudio_XASourceClose(&s_xacb.src);
			s_xacb.srcOpen = 0;
			s_xacb.fileNumber++;
			return 0;
		}

		s_vce.readSectors += (u32)chunk;
		s_xacb.pos += chunk;
		if (s_xacb.pos < s_xacb.span)
		{
			return 0;
		}
	}

	/* Span fully read (or zero-length): de-interleave wanted tracks. */
	{
		int i;

		for (i = 0; i < s_xacb.numWanted; i++)
		{
			const struct NativeAudioXaTrackInfo *info = &s_xaVoiceCacheScratch[i].info;
			struct NativeAudioXaVoiceCacheTrack *track;
			int trackSpan;
			int matched = 0;
			int s;

			if (info->fileNumber != s_xacb.fileNumber)
			{
				continue;
			}

			trackSpan = (info->numSectors > s_xacb.src.totalSectors) ? s_xacb.src.totalSectors : info->numSectors;
			for (s = 0; s < trackSpan; s++)
			{
				if (NativeAudio_IsXAAudioSector(s_xacb.fileBuf + (size_t)s * (size_t)s_xacb.src.sectorSize, s_xacb.src.sectorBase, info->channelFilter))
				{
					matched++;
				}
			}
			if (matched <= 0)
			{
				continue;
			}

			s_xaAudioSectorCountMemo[info->entryIndex] = matched;

			if (s_xaVoiceCache.blobUsed + (u32)matched * (u32)s_xacb.src.sectorSize > NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES)
			{
				NativeAudio_XAVoiceCacheLog("[XAPRELOAD] budget full at cat=%d xa=%d (%u KiB) -- track stays on the SD path", s_xacb.categoryID,
				                            (int)s_xaVoiceCacheScratch[i].xaID, (unsigned)(s_xaVoiceCache.blobUsed >> 10));
				continue;
			}

			track = &s_xaVoiceCache.tracks[info->entryIndex];
			track->byteOffset = s_xaVoiceCache.blobUsed;
			track->sectorSize = (s16)s_xacb.src.sectorSize;
			track->sectorBase = (s16)s_xacb.src.sectorBase;
			for (s = 0; s < trackSpan; s++)
			{
				const u8 *sec = s_xacb.fileBuf + (size_t)s * (size_t)s_xacb.src.sectorSize;

				if (NativeAudio_IsXAAudioSector(sec, s_xacb.src.sectorBase, info->channelFilter))
				{
					memcpy(s_xaVoiceCache.blob + s_xaVoiceCache.blobUsed, sec, (size_t)s_xacb.src.sectorSize);
					s_xaVoiceCache.blobUsed += (u32)s_xacb.src.sectorSize;
				}
			}
			track->sectorCount = matched;
			s_vce.tracks += 1;
			s_vce.sectors += matched;
		}
	}

	free(s_xacb.fileBuf);
	s_xacb.fileBuf = NULL;
	NativeAudio_XASourceClose(&s_xacb.src);
	s_xacb.srcOpen = 0;
	s_xacb.fileNumber++;
	return 0;
}
#endif /* __3DS__ */

#if defined(__3DS__)
/* MUSIC tracks are not cached in RAM (11 MiB, and the voice cache is nearly
 * full), so each pays a whole-file audio-sector scan on its first open per
 * boot -- and MUSIC files are the big ones (S00.XA is 10416 sectors), woven
 * eight tracks to a file. That scan runs on the game thread, which also ticks
 * the sequencer, so the first final lap and the first results screen of a
 * boot hitched, and the first four race-intro stings each hitched once. Fill
 * s_xaAudioSectorCountMemo for EVERY MUSIC track here instead: one sequential
 * pass per physical file, counting all of that file's tracks in the same
 * pass, each bounded by its own XNF slot exactly as NativeAudio_XAStreamOpenTrack
 * bounds it (an unbounded count would report the whole channel's length for a
 * short track and the game would then play silence out to the fake end).
 * Resumable, so a caller can spread it across frames. The scan is the fallback
 * path: on the disc this port accepts, the table below fills the same memo with
 * the same values and no reads at all. */
#define NATIVE_AUDIO_MUSIC_WARM_MAX_TRACKS 32

/* Every MUSIC track's audio-sector count is a constant of the NTSC-U disc --
 * the only disc this port accepts -- and discovering them by scanning costs
 * ~48 MiB of game-thread reads. These are those constants, next to the XNF
 * fields that identify the disc they were taken from.
 *
 * A row is used only when the live XNF entry matches it field for field, and
 * the match is all-or-nothing: any other disc, or a re-cut XNF, falls through
 * to the scan rather than playing a track out to a wrong length. Regenerate a
 * changed disc's rows from the scan's own [XAWARM] music lines. */
struct NativeAudioMusicCountRow
{
	s16 xaID;
	s16 entryIndex;
	s16 fileNumber;
	s16 channelFilter;
	s16 slotSectors;  /* XNF numSectors: the bound the scan runs to */
	s16 audioSectors; /* what the scan returns, and what the memo stores */
};

internal const struct NativeAudioMusicCountRow s_mcTableNtscU[] = {
    {0, 0, 0, 0, 1162, 146},   {1, 1, 0, 1, 1162, 146},   {2, 2, 0, 2, 1162, 145},   {3, 3, 0, 3, 1162, 145},  {4, 4, 0, 4, 10337, 1292},
    {5, 5, 0, 5, 10337, 1292}, {6, 6, 0, 6, 252, 31},     {7, 7, 1, 0, 2378, 298},   {8, 8, 1, 1, 2375, 297},  {9, 9, 1, 2, 2410, 301},
    {10, 10, 1, 3, 2375, 297}, {11, 11, 1, 4, 2385, 298}, {12, 12, 1, 5, 2403, 300},
};

internal u8 s_mcwChunkBuf[NATIVE_DISC_IMAGE_CHUNK_SECTORS * XA_FULL_SECTOR_SIZE];
internal struct
{
	b32 active;
	b32 srcOpen;
	int numTracks;
	u32 filesWanted;
	int warmed;
	int fileNumber;
	struct NativeAudioXaSource src;
	int maxBound;
	int sector;
	struct
	{
		int xaID;
		int bound; /* scan bound for this track within its file, in sectors */
		int count;
		struct NativeAudioXaTrackInfo info;
	} tracks[NATIVE_AUDIO_MUSIC_WARM_MAX_TRACKS];
} s_mcw;

/* Fills the count memo from the table when every wanted MUSIC track matches its
 * row. All-or-nothing on purpose: a partial match means the XNF is not the one
 * these counts came from, and a half-table memo is worse than either half. */
internal int NativeAudio_MusicCountTableFill(void)
{
	int t;

	if (s_mcw.numTracks != (int)len(s_mcTableNtscU))
	{
		return 0;
	}

	for (t = 0; t < s_mcw.numTracks; t++)
	{
		const struct NativeAudioMusicCountRow *row = &s_mcTableNtscU[t];
		const struct NativeAudioXaTrackInfo *info = &s_mcw.tracks[t].info;

		if ((s_mcw.tracks[t].xaID != row->xaID) || (info->entryIndex != row->entryIndex) || (info->fileNumber != row->fileNumber) ||
		    (info->channelFilter != row->channelFilter) || (info->numSectors != row->slotSectors))
		{
			return 0;
		}
	}

	for (t = 0; t < s_mcw.numTracks; t++)
	{
		s_xaAudioSectorCountMemo[s_mcw.tracks[t].info.entryIndex] = s_mcTableNtscU[t].audioSectors;
	}
	return 1;
}

internal void NativeAudio_MusicCountWarmBegin(void)
{
	int xaID;

	memset(&s_mcw, 0, sizeof(s_mcw));

	for (xaID = 0; (xaID < NATIVE_AUDIO_XA_TRACK_INDEX_MAX) && (s_mcw.numTracks < NATIVE_AUDIO_MUSIC_WARM_MAX_TRACKS); xaID++)
	{
		struct NativeAudioXaTrackInfo info;

		if (!NativeAudio_LookupXATrackInfo(NATIVE_AUDIO_XA_CATEGORY_MUSIC, xaID, &info))
		{
			continue;
		}
		if ((info.fileNumber < 0) || (info.fileNumber >= 32) || (info.entryIndex < 0) || (info.entryIndex >= NATIVE_AUDIO_XA_TRACK_INDEX_MAX))
		{
			continue;
		}
		s_mcw.tracks[s_mcw.numTracks].xaID = xaID;
		s_mcw.tracks[s_mcw.numTracks].info = info;
		s_mcw.numTracks++;
		s_mcw.filesWanted |= 1u << info.fileNumber;
	}

	if (NativeAudio_MusicCountTableFill())
	{
		NativeAudio_XAVoiceCacheLog("[XAWARM] music counts from table (%d/%d)", s_mcw.numTracks, s_mcw.numTracks);
		/* Step returns done on its first call: no MUSIC file is ever opened. */
		s_mcw.warmed = s_mcw.numTracks;
		s_mcw.active = 0;
		return;
	}

	NativeAudio_XAVoiceCacheLog("[XAWARM] music count table mismatch, scanning");
	s_mcw.active = 1;
}

internal void NativeAudio_MusicCountWarmFinishFile(void)
{
	int t;

	for (t = 0; t < s_mcw.numTracks; t++)
	{
		if ((s_mcw.tracks[t].info.fileNumber != s_mcw.fileNumber) || (s_mcw.tracks[t].count <= 0))
		{
			continue;
		}
		s_xaAudioSectorCountMemo[s_mcw.tracks[t].info.entryIndex] = s_mcw.tracks[t].count;
		s_mcw.warmed++;
		NativeAudio_XAVoiceCacheLog("[XAWARM] music xa=%d entry=%d file=S%02d ch=%d slot=%d sectors=%d", s_mcw.tracks[t].xaID,
		                            s_mcw.tracks[t].info.entryIndex, s_mcw.fileNumber, s_mcw.tracks[t].info.channelFilter,
		                            s_mcw.tracks[t].info.numSectors, s_mcw.tracks[t].count);
	}
	NativeAudio_XASourceClose(&s_mcw.src);
	s_mcw.srcOpen = 0;
	s_mcw.fileNumber++;
}

/* Advances the music warm pass by at most `budgetSectors` sectors of disc/SD
 * I/O (0 = uncapped, still bounded by NATIVE_DISC_IMAGE_CHUNK_SECTORS per
 * call). Returns nonzero once every wanted MUSIC file has been scanned. */
internal int NativeAudio_MusicCountWarmStep(int budgetSectors)
{
	int chunk;
	int i;

	if (!s_mcw.active)
	{
		return 1;
	}

	while (!s_mcw.srcOpen)
	{
		char path[128];
		int t;

		if (s_mcw.fileNumber >= 32)
		{
			NativeAudio_XAVoiceCacheLog("[XAWARM] music count memo warmed for %d of %d tracks", s_mcw.warmed, s_mcw.numTracks);
			s_mcw.active = 0;
			return 1;
		}
		if ((s_mcw.filesWanted & (1u << s_mcw.fileNumber)) == 0)
		{
			s_mcw.fileNumber++;
			continue;
		}
		if (!NativeAudio_BuildXAPath(path, sizeof(path), NATIVE_AUDIO_XA_CATEGORY_MUSIC, s_mcw.fileNumber) ||
		    !NativeAudio_XASourceOpen(&s_mcw.src, path, 0))
		{
			NativeAudio_XAVoiceCacheLog("[XAWARM] music file S%02d.XA: open failed -- its tracks scan on first play", s_mcw.fileNumber);
			s_mcw.fileNumber++;
			continue;
		}
		if ((s_mcw.src.sectorSize <= 0) || (s_mcw.src.sectorSize > XA_FULL_SECTOR_SIZE))
		{
			NativeAudio_XASourceClose(&s_mcw.src);
			s_mcw.fileNumber++;
			continue;
		}

		s_mcw.maxBound = 0;
		for (t = 0; t < s_mcw.numTracks; t++)
		{
			int bound;

			if (s_mcw.tracks[t].info.fileNumber != s_mcw.fileNumber)
			{
				continue;
			}
			bound = s_mcw.src.totalSectors;
			if ((s_mcw.tracks[t].info.numSectors > 0) && (s_mcw.tracks[t].info.numSectors < bound))
			{
				bound = s_mcw.tracks[t].info.numSectors;
			}
			s_mcw.tracks[t].bound = bound;
			if (bound > s_mcw.maxBound)
			{
				s_mcw.maxBound = bound;
			}
		}
		s_mcw.sector = 0;
		s_mcw.srcOpen = 1;
	}

	chunk = s_mcw.maxBound - s_mcw.sector;
	if (chunk <= 0)
	{
		NativeAudio_MusicCountWarmFinishFile();
		return 0;
	}
	if (chunk > NATIVE_DISC_IMAGE_CHUNK_SECTORS)
	{
		chunk = NATIVE_DISC_IMAGE_CHUNK_SECTORS;
	}
	if ((budgetSectors > 0) && (chunk > budgetSectors))
	{
		chunk = budgetSectors;
	}

	if (!NativeAudio_XASourceReadSectors(&s_mcw.src, s_mcw.sector, chunk, s_mcwChunkBuf, &s_xaDiscScratchPending))
	{
		/* short read: stop scanning this file, finalize whatever landed */
		s_mcw.sector = s_mcw.maxBound;
		NativeAudio_MusicCountWarmFinishFile();
		return 0;
	}

	s_vce.readSectors += (u32)chunk;

	for (i = 0; i < chunk; i++)
	{
		const u8 *raw = &s_mcwChunkBuf[(size_t)i * (size_t)s_mcw.src.sectorSize];
		int index = s_mcw.sector + i;
		int t;

		for (t = 0; t < s_mcw.numTracks; t++)
		{
			if ((s_mcw.tracks[t].info.fileNumber == s_mcw.fileNumber) && (index < s_mcw.tracks[t].bound) &&
			    NativeAudio_IsXAAudioSector(raw, s_mcw.src.sectorBase, s_mcw.tracks[t].info.channelFilter))
			{
				s_mcw.tracks[t].count++;
			}
		}
	}
	s_mcw.sector += chunk;

	if (s_mcw.sector >= s_mcw.maxBound)
	{
		NativeAudio_MusicCountWarmFinishFile();
	}
	return 0;
}
#endif

/* Orchestrates the resumable boot XA warm: MUSIC sector-count memo, then the
 * GAME and EXTRA voice-line RAM caches, in that order so the caches (bigger,
 * budget-limited) never crowd out the memo that every MUSIC open depends on.
 * Self-gates to a no-op after the first Begin (and on PC). The memo half is
 * table-fed and costs no reads unless the disc's XNF is an unknown one. */
void NativeAudio_VoiceCacheEnsureBegin(void)
{
#if defined(__3DS__)
	local_persist b32 s_voiceCacheTried;

	if (s_voiceCacheTried)
	{
		return;
	}
	s_voiceCacheTried = 1;

	memset(&s_vce, 0, sizeof(s_vce));
	s_vce.active = 1;

	/* The language chosen at boot (LOAD_LangFile has already run) decides whose voices are cached. */
	s_xaCachePack = NativeAudio_EffectiveVoicePack(s_voicePack);
	if (s_xaCachePack != 0)
	{
		NativeAudio_XAVoiceCacheLog("[PALVOICE] caching the %s voice pack", NativeAudio_PalPackCode(s_xaCachePack));
	}

	if (!N3DS_IsNew3DS())
	{
		s_xaVoiceCacheMaxBytes = NativeAudio_XAVoiceCacheOld3DSBytes();
		s_xaCacheRaceStings = 1;
	}
	N3DS_LogMemory("before voice cache");
	s_xaVoiceCache.blob = (NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES > 0) ? (u8 *)malloc(NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES) : NULL;
	if (s_xaVoiceCache.blob == NULL)
	{
		NativeAudio_XAVoiceCacheLog("[XAPRELOAD] no %u KiB for the voice cache -- voice lines stay on the SD path",
		                            (unsigned)(NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES >> 10));
	}

	/* Independent of the RAM cache below (MUSIC is never cached): fills the
	 * per-track sector-count memo so no MUSIC open ever scans mid-game. */
	NativeAudio_MusicCountWarmBegin();

	/* Read plan for the whole warm, so a caller holding the screen can show
	 * honest progress. The memo half costs nothing while its table matches
	 * the disc; only the fallback scan reads. */
	if (s_xaVoiceCache.blob != NULL)
	{
		s_vce.plannedSectors = NativeAudio_XAVoiceCachePlannedSectors(NATIVE_AUDIO_XA_CATEGORY_GAME) +
		                       NativeAudio_XAVoiceCachePlannedSectors(NATIVE_AUDIO_XA_CATEGORY_EXTRA);
		if (s_xaCacheRaceStings)
		{
			s_vce.plannedSectors += NativeAudio_XAVoiceCachePlannedSectors(NATIVE_AUDIO_XA_CATEGORY_MUSIC);
		}
	}
	if (s_mcw.active)
	{
		int fileMax[32];
		int t;
		int f;

		memset(fileMax, 0, sizeof(fileMax));
		for (t = 0; t < s_mcw.numTracks; t++)
		{
			int file = s_mcw.tracks[t].info.fileNumber;

			if ((file >= 0) && (file < 32) && (s_mcw.tracks[t].info.numSectors > fileMax[file]))
			{
				fileMax[file] = s_mcw.tracks[t].info.numSectors;
			}
		}
		for (f = 0; f < 32; f++)
		{
			s_vce.plannedSectors += (u32)fileMax[f];
		}
	}
#endif
}

/* Disc/SD sectors the boot XA warm has read so far. A caller driving the warm
 * uses the change in this to tell a step that did I/O from one that only
 * opened a file or changed stage. Always 0 off the 3DS. */
u32 NativeAudio_VoiceCacheWarmSectorsRead(void)
{
#if defined(__3DS__)
	return s_vce.readSectors;
#else
	return 0;
#endif
}

/* 0..99 progress of the boot XA warm against its read plan, for a hold screen.
 * Capped below 100 because the plan is built from XNF slot sizes: a file
 * shorter than its slot reads less than planned, so only completion -- the
 * Step returning nonzero -- means done. Always 0 off the 3DS. */
int NativeAudio_VoiceCacheWarmPercent(void)
{
#if defined(__3DS__)
	u32 pct;

	if (s_vce.plannedSectors == 0)
	{
		return 0;
	}

	pct = (u32)(((u64)s_vce.readSectors * 100u) / s_vce.plannedSectors);
	return (pct > 99u) ? 99 : (int)pct;
#else
	return 0;
#endif
}

/* Call once per frame with a per-call sector budget (0 = uncapped) until it
 * returns nonzero. Safe to call after completion (returns 1 immediately). */
int NativeAudio_VoiceCacheEnsureStep(int budgetSectors)
{
#if defined(__3DS__)
	if (!s_vce.active)
	{
		return 1;
	}
	switch (s_vce.stage)
	{
	case 0:
		if (!NativeAudio_MusicCountWarmStep(budgetSectors))
		{
			return 0;
		}
		s_vce.stage = (s_xaVoiceCache.blob == NULL) ? 3 : (s_xaCacheRaceStings ? 4 : 1);
		if (s_vce.stage == 4)
		{
			NativeAudio_XAVoiceCacheBuildCategoryBegin(NATIVE_AUDIO_XA_CATEGORY_MUSIC);
		}
		else if (s_vce.stage == 1)
		{
			NativeAudio_XAVoiceCacheBuildCategoryBegin(NATIVE_AUDIO_XA_CATEGORY_GAME);
		}
		return 0;
	case 4:
		/* First, so the stings always fit the budget. */
		if (!NativeAudio_XAVoiceCacheBuildCategoryStep(budgetSectors))
		{
			return 0;
		}
		s_vce.stage = 1;
		NativeAudio_XAVoiceCacheBuildCategoryBegin(NATIVE_AUDIO_XA_CATEGORY_GAME);
		return 0;
	case 1:
		if (!NativeAudio_XAVoiceCacheBuildCategoryStep(budgetSectors))
		{
			return 0;
		}
		s_vce.stage = 2;
		NativeAudio_XAVoiceCacheBuildCategoryBegin(NATIVE_AUDIO_XA_CATEGORY_EXTRA);
		return 0;
	case 2:
		if (!NativeAudio_XAVoiceCacheBuildCategoryStep(budgetSectors))
		{
			return 0;
		}
		s_vce.stage = 3;
		break;
	default:
		break;
	}

	if (s_vce.tracks <= 0)
	{
		/* Nothing landed (unreadable XNF, all files failed, or no blob):
		 * give the 16 MiB back and leave `built` unset so opens fall through
		 * silently instead of logging a per-play miss for a cache that never
		 * existed. */
		if (s_xaVoiceCache.blob != NULL)
		{
			free(s_xaVoiceCache.blob);
			s_xaVoiceCache.blob = NULL;
		}
		NativeAudio_XAVoiceCacheLog("[XAPRELOAD] build produced no tracks -- voice lines stay on the SD path");
	}
	else
	{
		s_xaVoiceCache.built = 1;
		N3DS_LogMemory("voice cache built");
		NativeAudio_XAVoiceCacheLog("[XAPRELOAD] voice cache built: %d tracks, %d sectors, %u KiB of %u KiB budget", s_vce.tracks, s_vce.sectors,
		                            (unsigned)(s_xaVoiceCache.blobUsed >> 10), (unsigned)(NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES >> 10));
	}
	s_vce.active = 0;
	return 1;
#else
	(void)budgetSectors;
	return 1;
#endif
}

/* Refills the voice cache with the selected language's lines when they differ from the ones it holds.
 * Returns 1 when a refill started: drive it with NativeAudio_VoiceCacheEnsureStep until that returns
 * nonzero, starting no voice line in between (an open meanwhile takes the SD path). Returns 0 when the
 * cache already holds these voices, the boot warm has not finished, or there is no memory for it.
 * Main thread. */
int NativeAudio_VoiceCacheRebuildBegin(void)
{
#if defined(__3DS__)
	int pack = NativeAudio_EffectiveVoicePack(s_voicePack);
	struct NativeAudioXaClosedSource closed;

	if (s_vce.active || (pack == s_xaCachePack))
	{
		return 0;
	}

	/* A playing line backed by the cache would read the blob while it is rewritten. */
	closed.open = 0;
	NativeAudio_LockOutput();
	if (s_audio.xaStream.source.ramSectors != NULL)
	{
		closed = NativeAudio_FreeXA();
	}
	NativeAudio_UnlockOutput();
	if (closed.open)
	{
		NativeAudio_XASourceClose(&closed.source);
	}

	s_xaVoiceCache.built = 0;
	s_xaVoiceCache.blobUsed = 0;
	memset(s_xaVoiceCache.tracks, 0, sizeof(s_xaVoiceCache.tracks));
	s_xaCachePack = pack;

	if (s_xaVoiceCache.blob == NULL)
	{
		s_xaVoiceCache.blob = (NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES > 0) ? (u8 *)malloc(NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES) : NULL;
		if (s_xaVoiceCache.blob == NULL)
		{
			NativeAudio_XAVoiceCacheLog("[XAPRELOAD] no %u KiB to rebuild the voice cache -- voice lines stay on the SD path",
			                            (unsigned)(NATIVE_AUDIO_XA_VOICE_CACHE_MAX_BYTES >> 10));
			return 0;
		}
	}

	NativeAudio_XAVoiceCacheLog("[PALVOICE] rebuilding the voice cache for %s", (pack != 0) ? NativeAudio_PalPackCode(pack) : "the disc's English");

	/* From the cache stages on: the music counts the boot warm memoized do not depend on the language. */
	memset(&s_vce, 0, sizeof(s_vce));
	s_vce.active = 1;
	s_vce.stage = s_xaCacheRaceStings ? 4 : 1;
	s_vce.plannedSectors = NativeAudio_XAVoiceCachePlannedSectors(NATIVE_AUDIO_XA_CATEGORY_GAME) +
	                       NativeAudio_XAVoiceCachePlannedSectors(NATIVE_AUDIO_XA_CATEGORY_EXTRA);
	if (s_xaCacheRaceStings)
	{
		s_vce.plannedSectors += NativeAudio_XAVoiceCachePlannedSectors(NATIVE_AUDIO_XA_CATEGORY_MUSIC);
	}
	NativeAudio_XAVoiceCacheBuildCategoryBegin(s_xaCacheRaceStings ? NATIVE_AUDIO_XA_CATEGORY_MUSIC : NATIVE_AUDIO_XA_CATEGORY_GAME);
	return 1;
#else
	return 0;
#endif
}

// Core streaming primitive: reads and decodes exactly one more matching XA
// sector into the ring, entirely outside the lock, then takes
// NativeAudio_LockOutput() only for the final memcpy + counter bump. Skips
// non-audio sectors (multiplexed files) at negligible cost -- the shared
// ADPCM predictor state (decodeState) is untouched by skipped sectors,
// exactly like the old whole-file DecodeXAFile's per-sector "continue".
/* mayBlock 0 (game-thread ring only): returns -1 instead of reading the SD
 * card when the next sector is not read ahead yet. Otherwise 1 decoded, 0 at
 * the end of the stream or on a failure. */
internal int NativeAudio_XAStreamDecodeNextSectorEx(struct NativeAudioXaStream *stream, int mayBlock)
{
	struct NativeAudioPcmBuffer pcm;
	const u8 *header;
	int coding;
	int isStereo;
	int srBits;
	int sampleRate;
	int framesThisSector;
	int slot;

	if (!stream->open || stream->endOfStream)
	{
		return 0;
	}

	for (;;)
	{
		if (stream->nextPhysicalSector >= stream->source.totalSectors)
		{
			stream->endOfStream = 1;
			return 0;
		}

#if defined(__3DS__)
		/* Through the span cache: one ~75KB bulk read per 32 sectors instead
		 * of a FAT-mutex crossing per 2.3KB sector -- the measured 73%-of-
		 * worker cost this session exists to remove. */
		int got = NativeAudio_XAStreamReadSectorBuffered(stream, stream->nextPhysicalSector, stream->sectorBuf, mayBlock);

		if (got < 0)
		{
			return -1;
		}
		if (got == 0)
#else
		(void)mayBlock;
		if (!NativeAudio_XASourceReadSector(&stream->source, stream->nextPhysicalSector, stream->sectorBuf, stream->discScratch))
#endif
		{
			stream->endOfStream = 1;
			return 0;
		}

		if (NativeAudio_IsXAAudioSector(stream->sectorBuf, stream->source.sectorBase, stream->source.channelFilter))
		{
			break;
		}

		stream->nextPhysicalSector++;
	}

	header = &stream->sectorBuf[stream->source.sectorBase];
	coding = header[3];
	isStereo = (coding & 0x03) != 0;
	srBits = (coding >> 2) & 0x03;
	sampleRate = (srBits == 0) ? XA_SAMPLE_RATE_37800 : XA_SAMPLE_RATE_18900;

	memset(&pcm, 0, sizeof(pcm));
	pcm.samples = stream->tempFrames;
	pcm.capacity = NATIVE_AUDIO_XA_SECTOR_FRAMES * NATIVE_AUDIO_CHANNELS;

	if (isStereo)
	{
		if (!NativeAudio_DecodeXASectorStereo(stream->sectorBuf, stream->source.sectorBase, &stream->decodeState, &pcm))
		{
			stream->endOfStream = 1;
			return 0;
		}
		framesThisSector = pcm.count / NATIVE_AUDIO_CHANNELS; // 2016: stereo decode already interleaves L/R
	}
	else
	{
		int i;

		if (!NativeAudio_DecodeXASectorMono(stream->sectorBuf, stream->source.sectorBase, &stream->decodeState, &pcm))
		{
			stream->endOfStream = 1;
			return 0;
		}
		framesThisSector = pcm.count; // 4032: duplicated below, count stays the frame count
		for (i = pcm.count; i-- > 0;)
		{
			stream->tempFrames[i * 2] = stream->tempFrames[i];
			stream->tempFrames[i * 2 + 1] = stream->tempFrames[i];
		}
	}

	if (stream->sampleRate == 0)
	{
		// Pin format from the first matching sector -- real tracks are
		// uniformly mono or stereo throughout, so later sectors just reuse
		// this instead of re-deriving it (which the old DecodeXAFile did
		// per-sector, harmlessly, since the value never actually changed).
		stream->sampleRate = sampleRate;
		stream->framesPerSector = framesThisSector;
	}

	slot = stream->decodedSectorCount % NATIVE_AUDIO_XA_RING_SECTORS;

	NativeAudio_LockOutput();
	memcpy(&stream->ringPcm[(size_t)slot * NATIVE_AUDIO_XA_SECTOR_FRAMES * NATIVE_AUDIO_CHANNELS], stream->tempFrames,
	       (size_t)stream->framesPerSector * NATIVE_AUDIO_CHANNELS * sizeof(s16));
	stream->nextPhysicalSector++;
	stream->decodedSectorCount++;
	NativeAudio_UnlockOutput();

	return 1;
}

internal int NativeAudio_XAStreamDecodeNextSector(struct NativeAudioXaStream *stream)
{
	return NativeAudio_XAStreamDecodeNextSectorEx(stream, 1);
}

// Replaces direct PCM indexing everywhere. sector is a LOGICAL (decoded-so-
// far) sector index, not the physical read cursor -- see decodedSectorCount's
// comment on struct NativeAudioXaStream for why the two can diverge. Out of
// the resident window returns silence (same graceful contract as the old
// pcm==NULL / frameIndex>=frameCount guard) and bumps underrunEvents.
internal int NativeAudio_XARingReadSampleNoLock(struct NativeAudioXaStream *stream, int channel, u64 frameIndex)
{
	int sector;
	int frameInSector;
	int oldestResidentSector;
	int slot;

	if (!stream->open || (stream->framesPerSector <= 0))
	{
		return 0;
	}

	sector = (int)(frameIndex / (u64)stream->framesPerSector);
	frameInSector = (int)(frameIndex % (u64)stream->framesPerSector);

	oldestResidentSector = stream->decodedSectorCount - NATIVE_AUDIO_XA_RING_SECTORS;
	if ((sector < oldestResidentSector) || (sector >= stream->decodedSectorCount))
	{
		stream->underrunEvents++;
		return 0;
	}

	slot = sector % NATIVE_AUDIO_XA_RING_SECTORS;
	return stream->ringPcm[((size_t)slot * NATIVE_AUDIO_XA_SECTOR_FRAMES + (size_t)frameInSector) * NATIVE_AUDIO_CHANNELS + (size_t)channel];
}

#if defined(__3DS__)
/*
 * Bulk sibling of the per-sample reader above: copies a contiguous span of
 * decoded stereo frames straight out of the ring, under the same residency
 * contract (outside the resident window yields silence and bumps
 * underrunEvents).
 *
 * Why it exists. The per-sample reader divides AND modulos a u64 by the
 * runtime framesPerSector on every single access, and the 44.1kHz zig-zag
 * resampler called it 29 taps x 2 channels x 735 frames = 42,630 times per
 * parcel -- 42,630 software __aeabi_uldivmod calls per 16.7ms of music on a
 * CPU with no divide instruction, plus 42,630 scattered reads across a ~1MB
 * ring. That parcel measured 52-56ms on hardware (1,670 parcels, two runs):
 * production ran at a third of real time, which is what
 * the owner heard as the announcer "talking into a fan". Here the divide
 * happens once per sector the span touches -- one or two per parcel -- and
 * the samples move by memcpy.
 *
 * 3DS-only on purpose: PC keeps the per-sample resampler and stays
 * byte-identical (the mixer golden test gates that).
 */
internal void NativeAudio_XARingCopySpanNoLock(struct NativeAudioXaStream *stream, s16 *dst, u64 frameIndex, int frameCount)
{
	int remaining = frameCount;

	if ((dst == NULL) || (frameCount <= 0))
	{
		return;
	}
	if (!stream->open || (stream->framesPerSector <= 0) || (stream->ringPcm == NULL))
	{
		memset(dst, 0, (size_t)frameCount * NATIVE_AUDIO_CHANNELS * sizeof(s16));
		return;
	}

	while (remaining > 0)
	{
		int sector = (int)(frameIndex / (u64)stream->framesPerSector);
		int frameInSector = (int)(frameIndex % (u64)stream->framesPerSector);
		int oldestResidentSector = stream->decodedSectorCount - NATIVE_AUDIO_XA_RING_SECTORS;
		int chunk = stream->framesPerSector - frameInSector;

		if (chunk > remaining)
		{
			chunk = remaining;
		}

		if ((sector < oldestResidentSector) || (sector >= stream->decodedSectorCount))
		{
			/* One underrun event per non-resident slice instead of per
			 * sample: the counter's units change, its meaning (the ring
			 * could not serve playback) does not. */
			stream->underrunEvents++;
			memset(dst, 0, (size_t)chunk * NATIVE_AUDIO_CHANNELS * sizeof(s16));
		}
		else
		{
			int slot = sector % NATIVE_AUDIO_XA_RING_SECTORS;
			const s16 *src = stream->ringPcm + ((size_t)slot * NATIVE_AUDIO_XA_SECTOR_FRAMES + (size_t)frameInSector) * NATIVE_AUDIO_CHANNELS;

			memcpy(dst, src, (size_t)chunk * NATIVE_AUDIO_CHANNELS * sizeof(s16));
		}

		dst += (size_t)chunk * NATIVE_AUDIO_CHANNELS;
		frameIndex += (u64)chunk;
		remaining -= chunk;
	}
}
#endif

internal int NativeAudio_XAStreamAheadCountNoLock(const struct NativeAudioXaStream *stream)
{
	int currentSector;
	int ahead;

	if (!stream->open || (stream->framesPerSector <= 0))
	{
		return 0;
	}

	currentSector = (int)((s_audio.xa.positionFp >> NATIVE_AUDIO_FP_SHIFT) / (u64)stream->framesPerSector);
	ahead = stream->decodedSectorCount - currentSector;
	return (ahead < 0) ? 0 : ahead;
}

// Called once per NativeAudio_StepVBlank, right after its existing
// NativeAudio_UnlockOutput(): reads the current playback sector under a brief
// lock, and if resident-ahead count is below NATIVE_AUDIO_XA_RING_AHEAD_TARGET,
// decodes exactly one more sector. Only drives xaStream (the live one) --
// xaPendingStream is only filled by the priming/seek bursts in the Open*
// functions below.
internal void NativeAudio_XAStreamPump(void)
{
	int active;
	int ahead;

	NativeAudio_LockOutput();
	active = s_audio.xa.active;
	ahead = NativeAudio_XAStreamAheadCountNoLock(&s_audio.xaStream);
	NativeAudio_UnlockOutput();

#if defined(__3DS__)
	/*
	 * Hysteresis refill: every SD sector read crosses the FAT driver's
	 * global mutex against main-thread bulk scene loads -- the session-20
	 * freeze forensics (136s race-end frame, 33.9s intro frame) fingered
	 * exactly that fine-grained interleaving. So instead of nibbling one
	 * sector whenever we dip below target, let the ring drain to a low
	 * water mark and refill toward target in one rare burst: SD contention
	 * episodes become ~10x less frequent, and the deep 3DS ring (32
	 * sectors) plus the ndsp parcel cushion ride out the episodes
	 * themselves. PC keeps single-sector-per-vblank below -- replay
	 * determinism depends on ring-fill timing staying as it always was.
	 */
	{
		static int s_refilling;
		int pumped;

		if (!active)
		{
			s_refilling = 0;
			return;
		}
		if (!N3DS_AudioThreadIsRunning())
		{
			/* Game thread: no burst. Keep one 16-sector read ahead of the
			 * decode, then decode at most one sector (3.2 vblanks of audio at
			 * the least) from what is read; block on the SD card only when the
			 * music channel is about to run dry. */
			struct NativeAudioXaStream *stream = &s_audio.xaStream;

			if (stream->open && !stream->endOfStream && (stream->source.ramSectors == NULL) && (stream->spanBuf != NULL))
			{
				int ringEnd = stream->spanStartSector + stream->spanSectorCount;
				int readAhead = ((stream->nextPhysicalSector >= stream->spanStartSector) && (stream->nextPhysicalSector < ringEnd))
				                    ? (ringEnd - stream->nextPhysicalSector)
				                    : 0;

				if (readAhead < NATIVE_AUDIO_XA_READAHEAD_SECTORS_INLINE)
				{
					NativeAudio_XASpanRingAppend(stream, NATIVE_AUDIO_XA_READAHEAD_SECTORS_INLINE);
				}
			}
			if (ahead < NATIVE_AUDIO_XA_AHEAD_TARGET_INLINE)
			{
				int starving = (ahead <= 1) && (N3DS_NdspGetQueuedBlocks() < NATIVE_AUDIO_XA_STARVE_PARCELS_INLINE);

				NativeAudio_XAStreamDecodeNextSectorEx(stream, starving);
			}
			return;
		}
		if (!s_refilling && (ahead >= (NATIVE_AUDIO_XA_RING_AHEAD_TARGET / 3)))
		{
			return; /* above low water; leave the SD card alone */
		}
		s_refilling = 1;

		/* 24 audio sectors per wake (was 8): with span reads one burst costs
		 * a handful of bulk SD ops rather than ~200 single-sector ones, and
		 * the deeper 64-slot ring gives the burst somewhere to go. Still
		 * bounded so one wake cannot monopolize the worker. */
		for (pumped = 0; pumped < 24; pumped++)
		{
			NativeAudio_XAStreamDecodeNextSector(&s_audio.xaStream);
			NativeAudio_LockOutput();
			active = s_audio.xa.active;
			ahead = NativeAudio_XAStreamAheadCountNoLock(&s_audio.xaStream);
			NativeAudio_UnlockOutput();
			if (!active || (ahead >= NATIVE_AUDIO_XA_RING_AHEAD_TARGET))
			{
				s_refilling = 0;
				break;
			}
		}
	}
#else
	if (!active || (ahead >= NATIVE_AUDIO_XA_RING_AHEAD_TARGET))
	{
		return;
	}

	NativeAudio_XAStreamDecodeNextSector(&s_audio.xaStream);
#endif
}

#if defined(__3DS__)
// Diagnostic helper for the 3DS stats block (native_renderer_3ds.c); reads
// without a lock, same as that block's pre-existing (unlocked) reads of
// s_audio.voicePcmArena.capacity -- a best-effort periodic counter, not a
// correctness-sensitive read. Guarded like NativeAudio_SetDeterministicRenderMode's
// 3DS-only call above: native_renderer.c (the PC backend) has no equivalent
// stats block, so this would otherwise be an unused-function warning on PC.
internal int NativeAudio_XAStreamSectorsBuffered(void)
{
	int ahead = NativeAudio_XAStreamAheadCountNoLock(&s_audio.xaStream);

	return (ahead > NATIVE_AUDIO_XA_RING_SECTORS) ? NATIVE_AUDIO_XA_RING_SECTORS : ahead;
}
#endif

// Opens + synchronously primes up to NATIVE_AUDIO_XA_RING_AHEAD_TARGET
// sectors (the one remaining bounded synchronous burst, now a small constant
// instead of a whole track). categoryID/xaID resolve via the XNF metadata,
// same as the old NativeAudio_LoadXATrackPcm.
internal int NativeAudio_XAStreamOpenTrack(struct NativeAudioXaStream *stream, int categoryID, int xaID, int *frameCountOut, int *sampleRateOut)
{
	struct NativeAudioXaTrackInfo info;
	char path[256];
	int audioSectors;
	int fromCache; /* 3DS voice cache hit; constant 0 elsewhere */
	int i;

	*frameCountOut = 0;
	*sampleRateOut = 0;

	if (!NativeAudio_LookupXATrackInfo(categoryID, xaID, &info))
	{
		return 0;
	}
	if (!NativeAudio_BuildXAPathForPack(path, sizeof(path), info.pack, categoryID, info.fileNumber))
	{
		return 0;
	}

	NativeAudio_XAStreamInitRingBackingIfNeeded();
	NativeAudio_XAStreamClearNoLock(stream);

	fromCache = 0;
#if defined(__3DS__)
	if (NativeAudio_XAVoiceCacheOpen(&stream->source, &info))
	{
		/* Preloaded at the loading screen: every stored sector matches the
		 * filter by construction, so the count IS the sector total and the
		 * whole open (and the pump after it) does zero disc reads. */
		fromCache = 1;
		audioSectors = stream->source.totalSectors;
	}
	else if (s_xaVoiceCache.built && NativeAudio_XAVoiceCacheWantsTrack(categoryID, xaID) && (info.pack == NativeAudio_XAVoiceCachePackFor(categoryID)))
	{
		/* A track the preloader should own is falling back to SD. This line
		 * is the preloader spec's regression alarm -- it should never print
		 * mid-race. */
		NativeAudio_XAVoiceCacheLog("[XAPRELOAD] miss cat=%d xa=%d -- streaming from SD", categoryID, xaID);
	}
#endif

	if (!fromCache)
	{
		if (!NativeAudio_XASourceOpenForPack(&stream->source, path, info.pack, info.channelFilter))
		{
			return 0;
		}
		if ((info.numSectors > 0) && (info.numSectors < stream->source.totalSectors))
		{
			stream->source.totalSectors = info.numSectors;
		}

		// info.numSectors bounds the PHYSICAL scan range, but some categories
		// share one physical file across many xaIDs, interleaved by channel
		// (e.g. CDSYS_XA_TYPE_EXTRA voice lines, like MM_Scrapbook.c's TEST.STR
		// case below). Treating totalSectors as frameCount overstates track
		// length for these files and can leave outputFrame chasing a target the
		// ring will never reach. Count real audio sectors matching
		// channelFilter instead, exactly like NativeAudio_XAStreamOpenFile
		// already does.
#if defined(__3DS__)
		if ((info.entryIndex >= 0) && (info.entryIndex < NATIVE_AUDIO_XA_KEY_MAX) && (s_xaAudioSectorCountMemo[info.entryIndex] > 0))
		{
			/* Same scan, same source, same bound -- the count cannot change
			 * within a boot, and for MUSIC-sized files re-walking the whole
			 * file per open was multi-second SD work. */
			audioSectors = s_xaAudioSectorCountMemo[info.entryIndex];
		}
		else
#endif
		{
#if defined(__3DS__)
			/* Regression alarm: the boot-time NativeAudio_MusicCountWarm should
			 * have memoized every MUSIC track, so a MUSIC scan here means the
			 * warm missed one and the game thread is about to walk a ~20 MiB
			 * file mid-game. Must never print. */
			if (categoryID == NATIVE_AUDIO_XA_CATEGORY_MUSIC)
			{
				NativeAudio_XAVoiceCacheLog("[XAWARM] late music scan cat=%d xa=%d (not pre-warmed)", categoryID, xaID);
			}
#endif
			audioSectors = NativeAudio_XAStreamCountAudioSectors(&stream->source);
#if defined(__3DS__)
			if ((audioSectors > 0) && (info.entryIndex >= 0) && (info.entryIndex < NATIVE_AUDIO_XA_KEY_MAX))
			{
				s_xaAudioSectorCountMemo[info.entryIndex] = audioSectors;
			}
#endif
		}
	}
	if (audioSectors <= 0)
	{
		NativeAudio_XASourceClose(&stream->source);
		NativeAudio_XAStreamClearNoLock(stream);
		return 0;
	}
	stream->open = 1;

	{
#if defined(__3DS__)
		int prime = N3DS_AudioThreadIsRunning() ? NATIVE_AUDIO_XA_PRIME_TARGET : NATIVE_AUDIO_XA_PRIME_TARGET_INLINE;
#else
		int prime = NATIVE_AUDIO_XA_PRIME_TARGET;
#endif

		for (i = 0; i < prime; i++) /* small synchronous prime; the pump fills to its ahead target */
		{
			if (!NativeAudio_XAStreamDecodeNextSector(stream))
			{
				break;
			}
		}
	}

	if (stream->decodedSectorCount == 0)
	{
		NativeAudio_XASourceClose(&stream->source);
		NativeAudio_XAStreamClearNoLock(stream);
		return 0;
	}

	*frameCountOut = audioSectors * stream->framesPerSector;
	*sampleRateOut = stream->sampleRate;
	return 1;
}

// TEST.STR on the NTSC-U disc: total sectors, and audio sectors on channel 1.
#define NATIVE_AUDIO_TEST_STR_SECTORS            44239
#define NATIVE_AUDIO_TEST_STR_CH1_AUDIO_SECTORS  5494

// Same as NativeAudio_XAStreamOpenTrack, but for an explicit relative path +
// channelFilter (used by MM_Scrapbook.c's TEST.STR playback) instead of
// resolving through the XNF track metadata.
internal int NativeAudio_XAStreamOpenFile(struct NativeAudioXaStream *stream, const char *relativePath, int channelFilter, int *frameCountOut,
                                          int *sampleRateOut)
{
	int audioSectors;
	int i;

	*frameCountOut = 0;
	*sampleRateOut = 0;

	if ((relativePath == NULL) || (channelFilter < 0) || (channelFilter > 0xff))
	{
		return 0;
	}

	NativeAudio_XAStreamInitRingBackingIfNeeded();
	NativeAudio_XAStreamClearNoLock(stream);

	if (!NativeAudio_XASourceOpen(&stream->source, relativePath, channelFilter))
	{
		return 0;
	}

	/* Counting reads every sector of the file on this (main) thread: about 20 s
	 * for the Scrapbook's TEST.STR on 3DS. On the one supported disc its
	 * channel-1 count is fixed, so a file of exactly that length uses it. */
	if ((strcmp(relativePath, "TEST.STR") == 0) && (channelFilter == 1) && (stream->source.totalSectors == NATIVE_AUDIO_TEST_STR_SECTORS))
	{
		audioSectors = NATIVE_AUDIO_TEST_STR_CH1_AUDIO_SECTORS;
	}
	else
	{
		audioSectors = NativeAudio_XAStreamCountAudioSectors(&stream->source);
	}
	if (audioSectors <= 0)
	{
		NativeAudio_XASourceClose(&stream->source);
		NativeAudio_XAStreamClearNoLock(stream);
		return 0;
	}
	stream->open = 1;

	for (i = 0; i < NATIVE_AUDIO_XA_PRIME_TARGET; i++) /* small synchronous prime; the worker pump fills to AHEAD_TARGET */
	{
		if (!NativeAudio_XAStreamDecodeNextSector(stream))
		{
			break;
		}
	}

	if (stream->decodedSectorCount == 0)
	{
		NativeAudio_XASourceClose(&stream->source);
		NativeAudio_XAStreamClearNoLock(stream);
		return 0;
	}

	*frameCountOut = audioSectors * stream->framesPerSector;
	*sampleRateOut = stream->sampleRate;
	return 1;
}

// Used only by RestoreState: opens and decodes sequentially from sector 0
// (required -- ADPCM predictor state is sequential) until the saved
// targetSourceFrame's sector is resident plus the normal ahead-cushion. A
// rare, load-time-only synchronous burst, not a steady-state cost.
internal int NativeAudio_XAStreamOpenTrackSeek(struct NativeAudioXaStream *stream, int categoryID, int xaID, u64 targetSourceFrame, int *frameCountOut,
                                               int *sampleRateOut)
{
	struct NativeAudioXaTrackInfo info;
	char path[256];
	int audioSectors;
	int targetSector;

	*frameCountOut = 0;
	*sampleRateOut = 0;

	if (!NativeAudio_LookupXATrackInfo(categoryID, xaID, &info))
	{
		return 0;
	}
	if (!NativeAudio_BuildXAPathForPack(path, sizeof(path), info.pack, categoryID, info.fileNumber))
	{
		return 0;
	}

	NativeAudio_XAStreamInitRingBackingIfNeeded();
	NativeAudio_XAStreamClearNoLock(stream);

	if (!NativeAudio_XASourceOpenForPack(&stream->source, path, info.pack, info.channelFilter))
	{
		return 0;
	}
	if ((info.numSectors > 0) && (info.numSectors < stream->source.totalSectors))
	{
		stream->source.totalSectors = info.numSectors;
	}

	// See the matching NOTE in NativeAudio_XAStreamOpenTrack: this file may
	// multiplex several xaIDs by channel, so the true frame count has to come
	// from an actual audio-sector count, not stream->source.totalSectors
	// (the physical scan boundary). Counted independently of how far the
	// seek loop below actually decodes.
	audioSectors = NativeAudio_XAStreamCountAudioSectors(&stream->source);
	if (audioSectors <= 0)
	{
		NativeAudio_XASourceClose(&stream->source);
		NativeAudio_XAStreamClearNoLock(stream);
		return 0;
	}
	stream->open = 1;

	targetSector = -1;
	for (;;)
	{
		if (!NativeAudio_XAStreamDecodeNextSector(stream))
		{
			break;
		}
		if ((targetSector < 0) && (stream->framesPerSector > 0))
		{
			targetSector = (int)(targetSourceFrame / (u64)stream->framesPerSector);
		}
		if ((targetSector >= 0) && (stream->decodedSectorCount > targetSector + NATIVE_AUDIO_XA_PRIME_TARGET))
		{
			break;
		}
	}

	if (stream->decodedSectorCount == 0)
	{
		NativeAudio_XASourceClose(&stream->source);
		NativeAudio_XAStreamClearNoLock(stream);
		return 0;
	}

	*frameCountOut = audioSectors * stream->framesPerSector;
	*sampleRateOut = stream->sampleRate;
	return 1;
}

internal void NativeAudio_MixSample(int *dstLeft, int *dstRight, int sampleLeft, int sampleRight)
{
	*dstLeft += sampleLeft;
	*dstRight += sampleRight;
}

internal int NativeAudio_GetQueuedFramesNoLock(void)
{
	const int frameBytes = (int)sizeof(s16) * NATIVE_AUDIO_CHANNELS;
	int queuedBytes;
	int queuedFrames = s_audio.output.scheduledFrameCount;

	if (s_audio.output.stream != NULL)
	{
		queuedBytes = SDL_GetAudioStreamQueued(s_audio.output.stream);
		if (queuedBytes > 0)
		{
			queuedFrames += queuedBytes / frameBytes;
		}
	}

	return queuedFrames;
}

internal void NativeAudio_AddUnderrunFramesNoLock(int frameCount)
{
#ifdef CTR_INTERNAL
	if (frameCount <= 0)
	{
		return;
	}

	if (frameCount > INT_MAX - s_audio.output.underrunFrames)
	{
		s_audio.output.underrunFrames = INT_MAX;
	}
	else
	{
		s_audio.output.underrunFrames += frameCount;
	}
#else
	(void)frameCount;
#endif
}

internal void NativeAudio_AddOverflowFramesNoLock(int frameCount)
{
#ifdef CTR_INTERNAL
	if (frameCount <= 0)
	{
		return;
	}

	if (frameCount > INT_MAX - s_audio.output.overflowFrames)
	{
		s_audio.output.overflowFrames = INT_MAX;
	}
	else
	{
		s_audio.output.overflowFrames += frameCount;
	}
#else
	(void)frameCount;
#endif
}

internal int NativeAudio_QueueRenderedFramesNoLock(const s16 *frames, int frameCount)
{
	const int frameSamples = NATIVE_AUDIO_CHANNELS;
	int framesQueued = 0;

	if ((frames == NULL) || (frameCount <= 0))
	{
		return 0;
	}

	while ((framesQueued < frameCount) && (s_audio.output.scheduledFrameCount < NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES))
	{
		int writeFrame = (s_audio.output.scheduledReadFrame + s_audio.output.scheduledFrameCount) % NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES;
		int writable = NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES - s_audio.output.scheduledFrameCount;
		int contiguous = NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES - writeFrame;
		int remaining = frameCount - framesQueued;
		int chunkFrames = remaining;

		if (chunkFrames > writable)
		{
			chunkFrames = writable;
		}
		if (chunkFrames > contiguous)
		{
			chunkFrames = contiguous;
		}

		memcpy(&s_audio.output.scheduledPcm[writeFrame * frameSamples], &frames[framesQueued * frameSamples], (size_t)chunkFrames * frameSamples * sizeof(s16));
		s_audio.output.scheduledFrameCount += chunkFrames;
		framesQueued += chunkFrames;
	}

	if (framesQueued < frameCount)
	{
		NativeAudio_AddOverflowFramesNoLock(frameCount - framesQueued);
	}

	return framesQueued;
}

internal int NativeAudio_DrainRenderedFramesNoLock(s16 *out, int frameCount)
{
	const int frameSamples = NATIVE_AUDIO_CHANNELS;
	int framesDrained = 0;

	if ((out == NULL) || (frameCount <= 0))
	{
		return 0;
	}

	while ((framesDrained < frameCount) && (s_audio.output.scheduledFrameCount > 0))
	{
		int contiguous = NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES - s_audio.output.scheduledReadFrame;
		int remaining = frameCount - framesDrained;
		int chunkFrames = remaining;

		if (chunkFrames > s_audio.output.scheduledFrameCount)
		{
			chunkFrames = s_audio.output.scheduledFrameCount;
		}
		if (chunkFrames > contiguous)
		{
			chunkFrames = contiguous;
		}

		memcpy(&out[framesDrained * frameSamples], &s_audio.output.scheduledPcm[s_audio.output.scheduledReadFrame * frameSamples],
		       (size_t)chunkFrames * frameSamples * sizeof(s16));

		s_audio.output.scheduledReadFrame = (s_audio.output.scheduledReadFrame + chunkFrames) % NATIVE_AUDIO_SCHEDULED_QUEUE_FRAMES;
		s_audio.output.scheduledFrameCount -= chunkFrames;
		framesDrained += chunkFrames;
	}

	return framesDrained;
}

internal void SDLCALL NativeAudio_StreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	const int frameBytes = (int)sizeof(s16) * NATIVE_AUDIO_CHANNELS;
	s16 out[NATIVE_AUDIO_VBLANK_FRAMES * NATIVE_AUDIO_CHANNELS];
	int framesNeeded;

	(void)userdata;
	(void)total_amount;

	if ((stream == NULL) || (additional_amount <= 0))
	{
		return;
	}

	framesNeeded = (additional_amount + frameBytes - 1) / frameBytes;
#ifdef CTR_INTERNAL
	if (framesNeeded > s_audio.output.callbackMaxRequestFrames)
	{
		s_audio.output.callbackMaxRequestFrames = framesNeeded;
	}
#endif

	while (framesNeeded > 0)
	{
		int chunkFrames = framesNeeded < NATIVE_AUDIO_VBLANK_FRAMES ? framesNeeded : NATIVE_AUDIO_VBLANK_FRAMES;
		int framesReady;

		if (s_audio.output.deterministicRenderMode)
		{
			// NOTE(aalhendi): Deterministic replay/TAS mode must advance audio
			// from its scheduler. SDL only drains already-rendered PCM here.
			framesReady = NativeAudio_DrainRenderedFramesNoLock(out, chunkFrames);
			if (framesReady < chunkFrames)
			{
				memset(&out[framesReady * NATIVE_AUDIO_CHANNELS], 0, (size_t)(chunkFrames - framesReady) * frameBytes);
				NativeAudio_AddUnderrunFramesNoLock(chunkFrames - framesReady);
			}
		}
		else
		{
			framesReady = NativeAudio_RenderFramesNoLock(out, chunkFrames);
			if (framesReady < chunkFrames)
			{
				memset(&out[framesReady * NATIVE_AUDIO_CHANNELS], 0, (size_t)(chunkFrames - framesReady) * frameBytes);
			}
		}

		if (!SDL_PutAudioStreamData(stream, out, chunkFrames * frameBytes))
		{
			NativeAudio_AddUnderrunFramesNoLock(chunkFrames);
			break;
		}

		framesNeeded -= chunkFrames;
	}
}

void NativeAudio_ClearOutputQueue(void)
{
	NativeAudio_LockOutput();

	NativeAudio_ClearOutputQueueNoLock();

	NativeAudio_UnlockOutput();
}

void NativeAudio_SetDeterministicRenderMode(int enabled)
{
	NativeAudio_LockOutput();

	if (s_audio.output.deterministicRenderMode != (enabled != 0))
	{
		s_audio.output.deterministicRenderMode = enabled != 0;
		NativeAudio_ClearOutputQueueNoLock();
	}

	NativeAudio_UnlockOutput();
}

int NativeAudio_IsDeterministicRenderMode(void)
{
	int enabled;

	NativeAudio_LockOutput();

	enabled = s_audio.output.deterministicRenderMode;

	NativeAudio_UnlockOutput();

	return enabled;
}

int NativeAudio_QueueRenderedFrames(const s16 *frames, int frameCount)
{
	int framesQueued;

	if (frameCount <= 0)
	{
		return 0;
	}

	NativeAudio_LockOutput();

	if (s_audio.output.deterministicRenderMode)
	{
		framesQueued = NativeAudio_QueueRenderedFramesNoLock(frames, frameCount);
	}
	else
	{
		framesQueued = 0;
	}

	NativeAudio_UnlockOutput();

	return framesQueued;
}

#ifdef CTR_INTERNAL
void NativeAudio_GetOutputStats(int *underrunFrames, int *overflowFrames, int *queuedFrames)
{
	NativeAudio_LockOutput();

	if (underrunFrames != NULL)
	{
		*underrunFrames = s_audio.output.underrunFrames;
	}
	if (overflowFrames != NULL)
	{
		*overflowFrames = s_audio.output.overflowFrames;
	}
	if (queuedFrames != NULL)
	{
		*queuedFrames = NativeAudio_GetQueuedFramesNoLock();
	}

	NativeAudio_UnlockOutput();
}

internal int NativeAudio_ShouldReportOutputStatsNoLock(int *underrunFrames, int *overflowFrames, int *queuedFrames)
{
	if (s_audio.output.reportVBlankCountdown > 0)
	{
		s_audio.output.reportVBlankCountdown--;
		return 0;
	}

	s_audio.output.reportVBlankCountdown = 60;

	*underrunFrames = s_audio.output.underrunFrames;
	*overflowFrames = s_audio.output.overflowFrames;
	*queuedFrames = NativeAudio_GetQueuedFramesNoLock();

	if ((*underrunFrames == 0) && (*overflowFrames == 0))
	{
		return 0;
	}

	if ((*underrunFrames == s_audio.output.lastReportedUnderrunFrames) && (*overflowFrames == s_audio.output.lastReportedOverflowFrames) &&
	    (s_audio.output.callbackMaxRequestFrames == s_audio.output.lastReportedCallbackMaxRequestFrames))
	{
		return 0;
	}

	s_audio.output.lastReportedUnderrunFrames = *underrunFrames;
	s_audio.output.lastReportedOverflowFrames = *overflowFrames;
	s_audio.output.lastReportedCallbackMaxRequestFrames = s_audio.output.callbackMaxRequestFrames;
	return 1;
}
#endif

int NativeAudio_GetStateSize(void)
{
	return (int)sizeof(struct NativeAudioSnapshot);
}

int NativeAudio_CaptureState(void *dst, int dstSize)
{
	struct NativeAudioSnapshot *snapshot = (struct NativeAudioSnapshot *)dst;
	int i;

	if ((dst == NULL) || (dstSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}

	NativeAudio_LockOutput();

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->magic = NATIVE_AUDIO_STATE_MAGIC;
	snapshot->version = NATIVE_AUDIO_STATE_VERSION;
	snapshot->size = sizeof(*snapshot);
	snapshot->init = s_audio.init;
	snapshot->muted = s_audio.muted;
	snapshot->spuAllocCursor = s_audio.spu.allocCursor;
	snapshot->reverbEnabled = s_audio.reverbEnabled;
	snapshot->cdMixEnabled = s_audio.cdMixEnabled;
	snapshot->cdReverbEnabled = s_audio.cdReverbEnabled;
	snapshot->reverbWorkAreaReserved = s_audio.reverbWorkAreaReserved;
	snapshot->spuTransferOffset = s_audio.spu.transferOffset;
	snapshot->masterVolumeLeft = s_audio.masterVolumeLeft;
	snapshot->masterVolumeRight = s_audio.masterVolumeRight;
	snapshot->reverbVoiceBits = s_audio.reverbVoiceBits;
	snapshot->reverbAttr = s_audio.reverbAttr;
	snapshot->reverb = s_audio.reverb;
	snapshot->commonAttr = s_audio.commonAttr;
	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		NativeAudio_CopyVoiceToState(&snapshot->voices[i], &s_audio.voices[i]);
	}
	NativeAudio_CopyXAToState(&snapshot->xa, &s_audio.xa);
	memcpy(snapshot->spuSampleMem, s_audio.spu.memory, sizeof(snapshot->spuSampleMem));

	NativeAudio_UnlockOutput();

	return 1;
}

int NativeAudio_RestoreState(const void *src, int srcSize)
{
	const struct NativeAudioSnapshot *snapshot = (const struct NativeAudioSnapshot *)src;
	struct NativeAudioXaClosedSource closed;
	int xaFrameCount = 0;
	int xaSampleRate = 0;
	int i;
	int restoreInit;
	int haveNewStream = 0;

	if ((src == NULL) || (srcSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->magic != NATIVE_AUDIO_STATE_MAGIC) || (snapshot->version != NATIVE_AUDIO_STATE_VERSION) || (snapshot->size != sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->spuAllocCursor < 0) || (snapshot->spuAllocCursor > NATIVE_AUDIO_SPU_MEMSIZE))
	{
		return 0;
	}
	if ((snapshot->spuTransferOffset < 0) || (snapshot->spuTransferOffset > NATIVE_AUDIO_SPU_MEMSIZE))
	{
		return 0;
	}
	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		if (!NativeAudio_ValidateVoiceSnapshot(&snapshot->voices[i]))
		{
			return 0;
		}
	}
	if (!NativeAudio_ValidateXASnapshot(&snapshot->xa))
	{
		return 0;
	}
	if (!NativeAudio_ValidateReverbSnapshot(&snapshot->reverb))
	{
		return 0;
	}

	restoreInit = snapshot->init != 0;
	if (restoreInit && !NativeAudio_OpenDevice())
	{
		return 0;
	}

	if (snapshot->xa.active && snapshot->xa.hasTrackIdentity)
	{
		// NOTE(aalhendi): outputFrame is in 44100Hz output-frame units; convert
		// to the native/source-frame domain the ring is indexed in, the same
		// way NativeAudio_UpdateXAPositionFromOutputFrameNoLock does.
		u64 targetPositionFp = ((snapshot->xa.outputFrame * (u64)snapshot->xa.sampleRate) << NATIVE_AUDIO_FP_SHIFT) / NATIVE_AUDIO_SAMPLE_RATE;
		u64 targetSourceFrame = targetPositionFp >> NATIVE_AUDIO_FP_SHIFT;

		if (!NativeAudio_XAStreamOpenTrackSeek(&s_audio.xaPendingStream, snapshot->xa.categoryID, snapshot->xa.xaID, targetSourceFrame, &xaFrameCount,
		                                       &xaSampleRate))
		{
			return 0;
		}
		haveNewStream = 1;
	}

	NativeAudio_LockOutput();

	NativeAudio_ResetVoicePcmArenaNoLock(0);
	closed = NativeAudio_FreeXA();

	s_audio.init = restoreInit;
	s_audio.muted = snapshot->muted;
	s_audio.spu.allocCursor = snapshot->spuAllocCursor;
	s_audio.reverbEnabled = snapshot->reverbEnabled;
	s_audio.cdMixEnabled = snapshot->cdMixEnabled;
	s_audio.cdReverbEnabled = snapshot->cdReverbEnabled;
	s_audio.reverbWorkAreaReserved = snapshot->reverbWorkAreaReserved;
	s_audio.masterVolumeLeft = snapshot->masterVolumeLeft;
	s_audio.masterVolumeRight = snapshot->masterVolumeRight;
	s_audio.reverbVoiceBits = snapshot->reverbVoiceBits;
	s_audio.reverbAttr = snapshot->reverbAttr;
	s_audio.reverb = snapshot->reverb;
	s_audio.commonAttr = snapshot->commonAttr;
	memcpy(s_audio.spu.memory, snapshot->spuSampleMem, sizeof(s_audio.spu.memory));
	s_audio.spu.transferOffset = snapshot->spuTransferOffset;
	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		NativeAudio_CopyStateToVoice(&s_audio.voices[i], &snapshot->voices[i]);
	}

	if (haveNewStream)
	{
		NativeAudio_XAStreamSwap(&s_audio.xaStream, &s_audio.xaPendingStream);
		s_audio.xa.frameCount = xaFrameCount;
		s_audio.xa.sampleRate = xaSampleRate;
		s_audio.xa.categoryID = snapshot->xa.categoryID;
		s_audio.xa.xaID = snapshot->xa.xaID;
		s_audio.xa.hasTrackIdentity = 1;
		s_audio.xa.active = snapshot->xa.active;
		s_audio.xa.outputFrame = snapshot->xa.outputFrame;
		s_audio.xa.stepFp = (u32)(((u64)xaSampleRate << NATIVE_AUDIO_FP_SHIFT) / NATIVE_AUDIO_SAMPLE_RATE);
		s_audio.xa.volumeLeft = snapshot->xa.volumeLeft;
		s_audio.xa.volumeRight = snapshot->xa.volumeRight;
		NativeAudio_UpdateXAPositionFromOutputFrameNoLock();
	}
	else
	{
		s_audio.xa.frameCount = snapshot->xa.frameCount;
		s_audio.xa.sampleRate = snapshot->xa.sampleRate;
		s_audio.xa.categoryID = snapshot->xa.categoryID;
		s_audio.xa.xaID = snapshot->xa.xaID;
		s_audio.xa.hasTrackIdentity = snapshot->xa.hasTrackIdentity;
		s_audio.xa.active = 0;
		s_audio.xa.positionFp = snapshot->xa.positionFp;
		s_audio.xa.outputFrame = snapshot->xa.outputFrame;
		s_audio.xa.stepFp = snapshot->xa.stepFp;
		s_audio.xa.volumeLeft = snapshot->xa.volumeLeft;
		s_audio.xa.volumeRight = snapshot->xa.volumeRight;
	}

	NativeAudio_ClearOutputQueueNoLock();

	NativeAudio_UnlockOutput();

	// The old stream is captured+closed regardless of which branch above ran
	// -- a restore can replace a track that was playing with one that wasn't.
	if (closed.open)
	{
		NativeAudio_XASourceClose(&closed.source);
	}

	return 1;
}

internal void NativeAudio_MixFrame(s16 *outLeft, s16 *outRight)
{
	int mixLeft = 0;
	int mixRight = 0;
	int reverbSendLeft = 0;
	int reverbSendRight = 0;
	int reverbWetLeft = 0;
	int reverbWetRight = 0;
	int voiceStopped = 0;
	int i;

#if defined(__3DS__)
	/*
	 * When the worker thread is alive, XA does NOT advance here: it runs on
	 * the worker's own wall-clock via NativeAudio_RenderXAParcelNoLock --
	 * real time regardless of frame rate, which is what the PS1's CD
	 * hardware did (per-vblank advance chopped the boot announcer into
	 * starved fragments whenever frames ran slow).
	 * With no worker the game thread runs that same parcel path
	 * (N3DS_AudioInlineStepVBlank), so this branch is reached only by a
	 * caller that renders through MixFrame directly.
	 */
	if (s_audio.xa.active && !N3DS_AudioThreadIsRunning())
#else
	if (s_audio.xa.active)
#endif
	{
		u64 outputFrameCount = NativeAudio_GetXAOutputFrameCount(s_audio.xa.frameCount, s_audio.xa.sampleRate);

		if ((outputFrameCount == 0) || (s_audio.xa.outputFrame >= outputFrameCount))
		{
			s_audio.xa.active = 0;
		}
		else if (!s_audio.cdMixEnabled)
		{
			NativeAudio_AdvanceXAOutputFrameNoLock();
		}
		else
		{
			int srcLeft = NativeAudio_GetXAMixSampleNoLock(0);
			int srcRight = NativeAudio_GetXAMixSampleNoLock(1);
			int left = NativeAudio_ApplyVolume(srcLeft, s_audio.xa.volumeLeft, s_audio.masterVolumeLeft);
			int right = NativeAudio_ApplyVolume(srcRight, s_audio.xa.volumeRight, s_audio.masterVolumeRight);

			NativeAudio_MixSample(&mixLeft, &mixRight, left, right);
			if (s_audio.cdReverbEnabled)
			{
				NativeAudio_MixSample(&reverbSendLeft, &reverbSendRight, NativeAudio_ApplyMasterVolume(srcLeft, s_audio.xa.volumeLeft),
				                      NativeAudio_ApplyMasterVolume(srcRight, s_audio.xa.volumeRight));
			}
			NativeAudio_AdvanceXAOutputFrameNoLock();
		}
	}

	if (!s_audio.muted)
	{
		for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
		{
			struct NativeAudioVoice *voice = &s_audio.voices[i];
			u64 sampleIndex;
			u32 stepFp;
			int sample;
			int left;
			int right;

			if (voice->active && (!NativeAudio_VoiceHasSample(voice) || voice->sampleDirty))
			{
				/* Same predicate as before, restructured from the old
				 * short-circuit chain so the decode/scan sits alone inside
				 * the voice-start perf bucket. */
				NativePerf_BeginScope(NATIVE_PERF_BUCKET_AUDIO_VOICE_START);
				if (!NativeAudio_UpdateVoiceSample(voice))
				{
					voiceStopped = 1;
				}
				NativePerf_EndScope(NATIVE_PERF_BUCKET_AUDIO_VOICE_START);
			}

			if (!voice->active || !NativeAudio_VoiceHasSample(voice) || voice->sampleCount <= 0 || voice->adsrPhase == NATIVE_AUDIO_ADSR_OFF)
			{
				continue;
			}

			if (voice->attr.pitch == 0)
			{
				NativeAudio_AdsrAdvance(voice);
				if (!voice->active || voice->adsrPhase == NATIVE_AUDIO_ADSR_OFF)
				{
					voiceStopped = 1;
				}
				continue;
			}

			sampleIndex = voice->positionFp >> NATIVE_AUDIO_FP_SHIFT;
			if (sampleIndex >= (u64)voice->sampleCount)
			{
				if (voice->loopEnabled && voice->loopEnd > voice->loopStart)
				{
					NativeAudio_WrapVoiceLoop(voice);
					sampleIndex = voice->positionFp >> NATIVE_AUDIO_FP_SHIFT;
				}
				else
				{
					NativeAudio_AdsrForceOff(voice);
					voiceStopped = 1;
					continue;
				}
			}

#if defined(__3DS__)
			/*
			 * Hardware voices (plan B): this voice is PLAYED by
			 * its own ndsp channel -- see NativeAudio_HwVoiceMirrorNoLock.
			 * The per-sample loop keeps only the lifecycle arithmetic (ADSR
			 * advance, position advance, loop wrap, stop detection) because
			 * the game polls that state to sequence itself; measured once
			 * already: skipping it stalls boot at mainGameState=0. Software
			 * generation at 44.1kHz measured ~4x over real time on this CPU
			 * even on a dedicated core -- that is why the DSP owns playback.
			 */
			(void)sample;
			(void)left;
			(void)right;
#else
			sample = NativeAudio_InterpolateVoiceSample(voice);
			sample = NativeAudio_ApplyAdsrEnvelope(sample, voice->adsrLevel);
			left = NativeAudio_ApplyVolume(sample, voice->attr.volume.left, s_audio.masterVolumeLeft);
			right = NativeAudio_ApplyVolume(sample, voice->attr.volume.right, s_audio.masterVolumeRight);
			NativeAudio_MixSample(&mixLeft, &mixRight, left, right);
			if (voice->reverb)
			{
				NativeAudio_MixSample(&reverbSendLeft, &reverbSendRight, NativeAudio_ApplyMasterVolume(sample, voice->attr.volume.left),
				                      NativeAudio_ApplyMasterVolume(sample, voice->attr.volume.right));
			}
#endif
			NativeAudio_AdsrAdvance(voice);
			if (!voice->active || voice->adsrPhase == NATIVE_AUDIO_ADSR_OFF)
			{
				voiceStopped = 1;
			}

			stepFp = ((u32)voice->attr.pitch << NATIVE_AUDIO_FP_SHIFT) / 0x1000u;
			voice->positionFp += stepFp;

			if (voice->loopEnabled && ((voice->positionFp >> NATIVE_AUDIO_FP_SHIFT) >= (u64)voice->loopEnd))
			{
				NativeAudio_WrapVoiceLoop(voice);
			}
		}

		if (voiceStopped)
		{
			NativeAudio_ReclaimVoicePcmArenaIfIdleNoLock();
		}
	}

#if defined(__3DS__)
	/*
	 * Voices are dry on their DSP channels (v1 ships without PS1 reverb --
	 * the DSP has its own aux effects to approximate it with later), and
	 * with voice generation gated above the only reverb send left is XA's,
	 * so this would be echoing the music alone at software-mixing prices.
	 */
	(void)reverbSendLeft;
	(void)reverbSendRight;
	(void)reverbWetLeft;
	(void)reverbWetRight;
#else
	NativeAudio_ReverbProcessNoLock(reverbSendLeft, reverbSendRight, &reverbWetLeft, &reverbWetRight);
	NativeAudio_MixSample(&mixLeft, &mixRight, NativeAudio_ApplyMasterVolume(reverbWetLeft, s_audio.masterVolumeLeft),
	                      NativeAudio_ApplyMasterVolume(reverbWetRight, s_audio.masterVolumeRight));
#endif

	*outLeft = (s16)NativeAudio_Clamp16(mixLeft);
	*outRight = (s16)NativeAudio_Clamp16(mixRight);
}

/*
 * ===================================================== lifecycle fast path ===
 *
 * On 3DS the DSP plays the voices (plan B), so MixFrame's inner
 * loop generates nothing -- it exists purely to advance the state the GAME
 * polls: ADSR level/phase, sample position, loop wraps, ENDX-by-way-of
 * voice->active. Hardware run 6 measured what that bookkeeping costs when
 * it is walked one sample at a time: 735 frames x up to 24 voices per
 * vblank, with the worker over its per-wake budget badly enough to drop
 * 7,661 vblanks in a single race and starve the music queue to zero.
 *
 * The observation that fixes it: between counter overflows the envelope
 * state machine does NOTHING. The counter is a fixed increment per sample
 * and the level only moves when it wraps past STEP_BIT, so the samples in
 * between are a multiply, not a loop -- and for slow rates (shift 0x1f
 * increments by 1, i.e. an overflow every 32,768 samples) that is over half
 * a second of samples currently being walked one function call at a time.
 * Position advance is likewise linear between loop wraps.
 *
 * So this walks EVENTS instead of samples: ask how many samples are provably
 * inert (NativeAudio_AdsrIdleSamples), clamp that to the next position event,
 * jump the whole run in constant time, then do the one sample that actually
 * does something the ordinary way. Identical state by construction rather
 * than by approximation -- the skipped samples are ones where the existing
 * code demonstrably writes nothing but `adsrCounter += increment` and
 * `positionFp += stepFp`.
 *
 * `maxBatch` caps a single jump. It exists for the differential self-test
 * (NativeAudio_LifecycleSelfTest): at maxBatch=1 this degenerates to exactly
 * the one-sample-at-a-time path, so the test can compare this function
 * against itself and isolate the batching logic. Production callers pass the
 * whole vblank.
 */
internal int NativeAudio_LifecycleClampToPositionEvent(const struct NativeAudioVoice *voice, u32 stepFp, int samples)
{
	u64 limitFp;
	u64 room;
	u64 n;

	if (stepFp == 0)
	{
		return samples;
	}

	/*
	 * Two position events can fire, and we stop at whichever comes first:
	 * the loop wrap at the end of the sample body (loopEnabled, loopEnd) and
	 * the past-the-end test at the top of the next one (sampleCount). Taking
	 * the min means neither is ever jumped over even when the scanned loop
	 * points sit outside the decoded extent.
	 */
	limitFp = (u64)voice->sampleCount << NATIVE_AUDIO_FP_SHIFT;
	if (voice->loopEnabled)
	{
		u64 loopEndFp = (u64)voice->loopEnd << NATIVE_AUDIO_FP_SHIFT;

		if (loopEndFp < limitFp)
		{
			limitFp = loopEndFp;
		}
	}

	if (voice->positionFp >= limitFp)
	{
		return 0;
	}

	/* Largest n with positionFp + n*stepFp still strictly below the limit. */
	room = limitFp - 1u - voice->positionFp;
	n = room / stepFp;

	return (n >= (u64)samples) ? samples : (int)n;
}

/* Bench-only accounting (NativeAudio_LifecycleBench): iterations of the
 * segment loop and samples actually consumed, so a divergence can be blamed
 * on the loop rather than guessed at. */
global_variable unsigned long s_lifecycleDebugIters;
global_variable unsigned long s_lifecycleDebugFrames;

internal void NativeAudio_LifecycleAdvanceVoice(struct NativeAudioVoice *voice, int frames, int maxBatch, int *voiceStopped)
{
	while (frames > 0)
	{
		s_lifecycleDebugIters++;
		u32 stepFp;
		u64 sampleIndex;
		u32 increment;
		int idle;

		if (voice->active && (!NativeAudio_VoiceHasSample(voice) || voice->sampleDirty))
		{
			NativePerf_BeginScope(NATIVE_PERF_BUCKET_AUDIO_VOICE_START);
			if (!NativeAudio_UpdateVoiceSample(voice))
			{
				*voiceStopped = 1;
			}
			NativePerf_EndScope(NATIVE_PERF_BUCKET_AUDIO_VOICE_START);
		}

		if (!voice->active || !NativeAudio_VoiceHasSample(voice) || voice->sampleCount <= 0 || voice->adsrPhase == NATIVE_AUDIO_ADSR_OFF)
		{
			/* MixFrame `continue`s here every remaining sample; nothing in a
			 * lifecycle-only pass can revive the voice mid-vblank, so the
			 * whole remainder is that same no-op. */
			return;
		}

		if (voice->attr.pitch == 0)
		{
			/* Envelope advances, position is frozen (MixFrame's pitch==0
			 * branch) -- no position event can end the run. */
			idle = NativeAudio_AdsrIdleSamples(voice, (frames < maxBatch) ? frames : maxBatch, &increment);
			if (idle > 0)
			{
				voice->adsrCounter += (u32)idle * increment;
				frames -= idle;
				s_lifecycleDebugFrames += (unsigned long)idle;
				continue;
			}

			NativeAudio_AdsrAdvance(voice);
			if (!voice->active || voice->adsrPhase == NATIVE_AUDIO_ADSR_OFF)
			{
				*voiceStopped = 1;
			}
			frames--;
			s_lifecycleDebugFrames++;
			continue;
		}

		sampleIndex = voice->positionFp >> NATIVE_AUDIO_FP_SHIFT;
		if (sampleIndex >= (u64)voice->sampleCount)
		{
			if (voice->loopEnabled && (voice->loopEnd > voice->loopStart))
			{
				NativeAudio_WrapVoiceLoop(voice);
			}
			else
			{
				NativeAudio_AdsrForceOff(voice);
				*voiceStopped = 1;
				return;
			}
		}

		stepFp = ((u32)voice->attr.pitch << NATIVE_AUDIO_FP_SHIFT) / 0x1000u;

		idle = NativeAudio_AdsrIdleSamples(voice, (frames < maxBatch) ? frames : maxBatch, &increment);
		if (idle > 0)
		{
			idle = NativeAudio_LifecycleClampToPositionEvent(voice, stepFp, idle);
		}
		if (idle > 0)
		{
			/* increment is 0 for an all-ones rate: counter untouched, which
			 * is exactly what RunEnvelopeStep's early return does. */
			voice->adsrCounter += (u32)idle * increment;
			voice->positionFp += (u64)idle * stepFp;
			frames -= idle;
			s_lifecycleDebugFrames += (unsigned long)idle;
			continue;
		}

		NativeAudio_AdsrAdvance(voice);
		if (!voice->active || voice->adsrPhase == NATIVE_AUDIO_ADSR_OFF)
		{
			*voiceStopped = 1;
		}

		voice->positionFp += stepFp;

		if (voice->loopEnabled && ((voice->positionFp >> NATIVE_AUDIO_FP_SHIFT) >= (u64)voice->loopEnd))
		{
			NativeAudio_WrapVoiceLoop(voice);
		}

		frames--;
		s_lifecycleDebugFrames++;
	}
}

/*
 * One vblank of voice lifecycle, no audio generated. Mirrors what MixFrame
 * does to voice state over `frames` samples on 3DS -- where its XA branch is
 * owned by the worker's wall-clock path, its per-sample generation is
 * compiled out, and its reverb chain is off -- so the two are interchangeable
 * for state purposes, which is what NativeAudio_LifecycleSelfTest checks.
 *
 * The arena reclaim runs once at the end rather than per sample: it only ever
 * fires when NO voice is live (see NativeAudio_ReclaimVoicePcmArenaIfIdle-
 * NoLock's guard loop), and once that is true no voice advances again this
 * vblank, so the two placements cannot differ.
 */
internal void NativeAudio_LifecycleVBlankNoLock(int frames, int maxBatch)
{
	int voiceStopped = 0;
	int i;

	if (!s_audio.init || s_audio.muted || (frames <= 0))
	{
		return;
	}

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		NativeAudio_LifecycleAdvanceVoice(&s_audio.voices[i], frames, maxBatch, &voiceStopped);
	}

	if (voiceStopped)
	{
		NativeAudio_ReclaimVoicePcmArenaIfIdleNoLock();
	}
}

internal int NativeAudio_RenderFramesNoLock(s16 *out, int frameCount)
{
	int frame;

	if ((out == NULL) || (frameCount <= 0) || (frameCount > INT_MAX / NATIVE_AUDIO_CHANNELS))
	{
		return 0;
	}

	for (frame = 0; frame < frameCount; frame++)
	{
		if (s_audio.init)
		{
			NativeAudio_MixFrame(&out[frame * NATIVE_AUDIO_CHANNELS], &out[frame * NATIVE_AUDIO_CHANNELS + 1]);
		}
		else
		{
			out[frame * NATIVE_AUDIO_CHANNELS] = 0;
			out[frame * NATIVE_AUDIO_CHANNELS + 1] = 0;
		}
	}

	return frameCount;
}

#if defined(__3DS__)
/*
 * Hardware-voice mirror (plan B): once per vblank, translate
 * the 24 software voices' state into DSP channel pokes. The lifecycle loop
 * in MixFrame remains the AUTHORITY on game-visible state (ADSR phases,
 * ENDX, position); the DSP channel is only the renderer. Because the DSP
 * free-runs, sound stays real-time even when frames (and therefore vblank
 * emission and this mirror) run slow -- the mirror just pokes less often.
 *
 * Envelope quantization: SetMix once per vblank steps the envelope at
 * 60Hz instead of per-sample. PS1 attack/release curves faster than one
 * vblank land on their target within one poke; audible difference is a
 * slightly harder edge on very fast attacks, accepted for v1.
 *
 * Caller holds the render lock (worker's hold span, or the fallback's
 * LockOutput bracket) -- same discipline as everything touching s_audio.
 */
internal void NativeAudio_HwVoiceMirrorNoLock(void)
{
	int i;

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		struct NativeAudioVoice *voice = &s_audio.voices[i];
		struct NativeAudioHwVoiceShadow *shadow = &s_hwVoiceShadow[i];
		int audible = voice->active && (voice->adsrPhase != NATIVE_AUDIO_ADSR_OFF) && NativeAudio_VoiceHasSample(voice);

		if (shadow->keyonPending)
		{
			shadow->keyonPending = 0;
			if (audible)
			{
				float rateHz = (44100.0f * (float)voice->attr.pitch) / 4096.0f;

				/* pitch 0 freezes a PS1 voice on its current sample; a
				 * 1Hz rate is the closest silent-ish stand-in. */
				if (rateHz < 1.0f)
				{
					rateHz = 1.0f;
				}
				N3DS_HwVoiceStart(i, voice->pcm, voice->sampleCount, voice->loopStart, voice->loopEnd, voice->loopEnabled ? 1 : 0,
				                  (int)(voice->positionFp >> NATIVE_AUDIO_FP_SHIFT), rateHz);
				shadow->started = 1;
				shadow->lastPitch = voice->attr.pitch;
				shadow->lastGainL = -999.0f; /* force the first SetMix below */
				shadow->lastGainR = -999.0f;
			}
		}

		if (!shadow->started)
		{
			continue;
		}

		if (!audible)
		{
			N3DS_HwVoiceStop(i);
			shadow->started = 0;
			continue;
		}

		if (voice->attr.pitch != shadow->lastPitch)
		{
			float rateHz = (44100.0f * (float)voice->attr.pitch) / 4096.0f;

			if (rateHz < 1.0f)
			{
				rateHz = 1.0f;
			}
			N3DS_HwVoiceSetRate(i, rateHz);
			shadow->lastPitch = voice->attr.pitch;
		}

		{
			/* Same three gain stages as the software mixer's
			 * ApplyAdsrEnvelope + ApplyVolume, in float: level/ADSR_MAX,
			 * then voice and master volumes at /0x8000 each (Q15 direct
			 * mode; negative volume keeps its sign = PS1 phase invert). */
			float level = s_audio.muted ? 0.0f : ((float)voice->adsrLevel / (float)NATIVE_AUDIO_ADSR_MAX);
			float gainL = level * ((float)voice->attr.volume.left / 32768.0f) * ((float)s_audio.masterVolumeLeft / 32768.0f);
			float gainR = level * ((float)voice->attr.volume.right / 32768.0f) * ((float)s_audio.masterVolumeRight / 32768.0f);

			if ((gainL != shadow->lastGainL) || (gainR != shadow->lastGainR))
			{
				N3DS_HwVoiceSetMix(i, gainL, gainR);
				shadow->lastGainL = gainL;
				shadow->lastGainR = gainR;
			}
		}
	}
}

/*
 * External-linkage entry point for n3ds_audio_thread.c's worker thread. That
 * file is its own translation unit (same isolation as n3ds_backend.c -- see
 * n3ds_backend.h) and cannot call NativeAudio_RenderFramesNoLock directly:
 * that function is `internal` (static, macros.h:74), scoped to this unity
 * TU, and its s16 buffer type plus NATIVE_AUDIO_VBLANK_FRAMES/
 * NATIVE_AUDIO_CHANNELS are game-side symbols n3ds_audio_thread.c cannot see
 * without including headers that collide with libctru (n3ds_backend.h's
 * documented three-way collision). This wrapper owns the scratch buffer
 * itself so nothing PSX-typed needs to cross that boundary -- the rendered
 * samples go to the ndsp sink as a plain short pointer, one vblank block at
 * a time (N3DS_NdspSubmitMusicParcel copies out of this stack buffer before
 * returning). Caller (the worker thread) holds s_audioRenderLock for the
 * duration, which is the same lock NativeAudio_LockOutput/UnlockOutput take
 * on this platform via the SDL_LockAudioStream/UnlockAudioStream shim -- so
 * this still runs under the equivalent of the normal lock discipline even
 * though it does not call those functions directly. A refused submit (sink
 * down, pool full) is deliberate sample loss, counted on the sink side --
 * the mixer's own state has already advanced identically either way.
 */
void NativeAudio_RenderVBlankForAudioThread(void)
{
	unsigned long long lifecycleStart = N3DS_AudioTicksNow();

	/*
	 * Lifecycle + mirror only -- no audio is generated here. Every frame
	 * MixFrame would have produced on this path was a hard zero (voices play
	 * on their own DSP channels, XA advances on the worker's wall clock), so
	 * this drops the 2,940-byte scratch buffer and its 735 clamped stores
	 * along with the mixing, and walks the voice state by events instead of
	 * per sample. Same state out, per NativeAudio_LifecycleSelfTest; hardware
	 * run 6's dropped-vblank count is what made the per-sample walk
	 * untenable.
	 */
	NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, NATIVE_AUDIO_VBLANK_FRAMES);
	s_workerProfile.lifecycleTicks += N3DS_AudioTicksNow() - lifecycleStart;

	{
		unsigned long long mirrorStart = N3DS_AudioTicksNow();

		NativeAudio_HwVoiceMirrorNoLock();
		s_workerProfile.mirrorTicks += N3DS_AudioTicksNow() - mirrorStart;
	}
}

/*
 * Drain this file's half of the worker profile (see
 * struct NativeAudioWorkerProfile). Hand-matched cross-TU contract, same as
 * the render/pump/parcel wrappers above: n3ds_audio_thread.c owns the
 * reporting and the tick->microsecond conversion, this side owns the two
 * buckets only it can see inside the render call.
 */
void NativeAudio_TakeWorkerProfileForAudioThread(unsigned long long *lifecycleTicks, unsigned long long *mirrorTicks)
{
	if (lifecycleTicks != NULL)
	{
		*lifecycleTicks = s_workerProfile.lifecycleTicks;
	}
	if (mirrorTicks != NULL)
	{
		*mirrorTicks = s_workerProfile.mirrorTicks;
	}

	s_workerProfile.lifecycleTicks = 0;
	s_workerProfile.mirrorTicks = 0;
}

/*
 * Wall-clock XA: called by the worker at real-time 60Hz, independent of
 * vblank emission (n3ds_audio_thread.c paces it by svcGetSystemTick).
 * Renders one vblank's worth of the XA/CD music stream and submits it to
 * the music channel; a starved game loop no longer starves the music.
 *
 * "Renders" is now nearly a memcpy. The parcel carries SOURCE
 * PCM at the XA rate (37800/18900) and the DSP resamples it in hardware,
 * per-channel, for free -- deleting the 29-tap zig-zag resampler that
 * measured 52-56ms per 16.7ms parcel on the ARM11 (the "announcer talking
 * into a fan" finding). What stays CPU-side is the
 * game-visible XA clock, advanced in one step per parcel instead of 735.
 * Volume moves to the channel (ndspChnSetMix) instead of every sample.
 * PC is untouched and still runs the resampler -- see
 * NativeAudio_XARingCopySpanNoLock.
 * Takes the render lock itself -- the worker calls this OUTSIDE its
 * lifecycle-drain lock span. Returns 1 if a parcel was submitted, 0 if XA
 * was idle/silent -- the worker uses the idle->active edge to prime a
 * small queue cushion.
 */
int NativeAudio_RenderXAParcelForAudioThread(void)
{
	s16 parcel[NATIVE_AUDIO_VBLANK_FRAMES * NATIVE_AUDIO_CHANNELS];
	u64 outputFrameCount;
	u64 sourceFrameStart;
	u64 sourceFrameEnd;
	int parcelFrames;
	int sampleRate;
	int cdMixEnabled;
	float gainLeft;
	float gainRight;

	NativeAudio_LockOutput();

	if (!s_audio.init || !s_audio.xa.active)
	{
		NativeAudio_UnlockOutput();
		return 0; /* no submit: the music channel just drains to silence */
	}

	/* Hoisted out of what used to be a 735-iteration loop: both operands are
	 * fixed for the whole parcel, so this was 735 identical u64 divides. */
	outputFrameCount = NativeAudio_GetXAOutputFrameCount(s_audio.xa.frameCount, s_audio.xa.sampleRate);
	if ((outputFrameCount == 0) || (s_audio.xa.outputFrame >= outputFrameCount))
	{
		s_audio.xa.active = 0;
		NativeAudio_UnlockOutput();
		return 0;
	}

	sampleRate = s_audio.xa.sampleRate;
	cdMixEnabled = s_audio.cdMixEnabled;
	sourceFrameStart = s_audio.xa.positionFp >> NATIVE_AUDIO_FP_SHIFT;

	if (!N3DS_AudioThreadIsRunning())
	{
		/* Game thread: a stream starts with one decoded sector and the pump
		 * adds one a vblank, so the top-up can reach past the decoded data.
		 * Hold the clock and the parcel until the pump catches up (the music
		 * channel's cushion plays meanwhile) instead of handing the DSP
		 * silence for audio that exists. Returns -1: not idle, just early. */
		const struct NativeAudioXaStream *stream = &s_audio.xaStream;
		u64 nextOutputFrame = s_audio.xa.outputFrame + NATIVE_AUDIO_VBLANK_FRAMES;
		u64 neededSourceEnd;
		u64 decodedFrames = (u64)stream->decodedSectorCount * (u64)stream->framesPerSector;

		if (nextOutputFrame > outputFrameCount)
		{
			nextOutputFrame = outputFrameCount;
		}
		neededSourceEnd = (sampleRate > 0) ? ((nextOutputFrame * (u64)sampleRate) / NATIVE_AUDIO_SAMPLE_RATE) : 0;
		if (stream->open && !stream->endOfStream && (decodedFrames < (u64)s_audio.xa.frameCount) && (neededSourceEnd > decodedFrames))
		{
			NativeAudio_UnlockOutput();
			return -1;
		}
	}

	/*
	 * Advance the game-visible XA clock by one vblank in a single step. The
	 * per-sample walk this replaces did exactly this, 735 times: increment
	 * outputFrame, clamp at the end of the track, re-derive positionFp. The
	 * clock's units do not change -- outputFrame still counts 44.1kHz output
	 * frames, which is what the game polls and what the stream pump's
	 * ahead-count is measured against.
	 */
	s_audio.xa.outputFrame += NATIVE_AUDIO_VBLANK_FRAMES;
	if (s_audio.xa.outputFrame >= outputFrameCount)
	{
		s_audio.xa.outputFrame = outputFrameCount;
		s_audio.xa.active = 0; /* next call reports idle, which flushes the tail */
	}
	NativeAudio_UpdateXAPositionFromOutputFrameNoLock();
	sourceFrameEnd = s_audio.xa.positionFp >> NATIVE_AUDIO_FP_SHIFT;

	if (!cdMixEnabled)
	{
		/* CD mix off: the clock still runs (the game polls it), nothing is
		 * audible. Reported as idle, exactly as the old path did by leaving
		 * anyContent clear. */
		NativeAudio_UnlockOutput();
		return 0;
	}

	/*
	 * The parcel is now the SOURCE frames one vblank of output covers, handed
	 * to the DSP at the source rate for it to resample in hardware. Because
	 * positionFp is derived from outputFrame by the same formula, the two
	 * clocks cannot drift: 735 output frames is exactly 630 source frames at
	 * 37800 and exactly 315 at 18900 (735 * 6/7 and 735 * 3/7).
	 */
	parcelFrames = (int)(sourceFrameEnd - sourceFrameStart);
	if (parcelFrames > NATIVE_AUDIO_VBLANK_FRAMES)
	{
		/* Only reachable above 44100Hz, which XA never is. Clamp (which would
		 * detune) rather than overrun the buffer. */
		parcelFrames = NATIVE_AUDIO_VBLANK_FRAMES;
	}
	if (parcelFrames <= 0)
	{
		NativeAudio_UnlockOutput();
		return 0;
	}

	NativeAudio_XARingCopySpanNoLock(&s_audio.xaStream, parcel, sourceFrameStart, parcelFrames);

	/*
	 * Volume rides the channel instead of every sample: the same two-stage
	 * Q15 gain NativeAudio_ApplyVolume computes (CD volume, then master),
	 * handed over once per parcel and poked into the DSP only on change.
	 */
	gainLeft = ((float)NativeAudio_VolumeScale(s_audio.xa.volumeLeft) * (float)NativeAudio_VolumeScale(s_audio.masterVolumeLeft)) /
	           ((float)NATIVE_AUDIO_DIRECT_VOL_MAX * (float)NATIVE_AUDIO_DIRECT_VOL_MAX);
	gainRight = ((float)NativeAudio_VolumeScale(s_audio.xa.volumeRight) * (float)NativeAudio_VolumeScale(s_audio.masterVolumeRight)) /
	            ((float)NATIVE_AUDIO_DIRECT_VOL_MAX * (float)NATIVE_AUDIO_DIRECT_VOL_MAX);

	NativeAudio_UnlockOutput();

	N3DS_NdspSubmitMusicParcel(parcel, parcelFrames, sampleRate, gainLeft, gainRight);

	return 1;
}
#endif

#if defined(__3DS__)
/*
 * External-linkage entry point for n3ds_audio_thread.c's worker thread, same
 * cross-TU reasoning as NativeAudio_RenderVBlankForAudioThread above: that
 * file is its own translation unit and cannot call NativeAudio_XAStreamPump
 * directly, because that function is `internal` (static, macros.h:74),
 * scoped to this unity TU. Unlike the render wrapper, no PSX-typed buffer
 * needs to cross the boundary at all -- NativeAudio_XAStreamPump is already
 * void->void, so this wrapper is a pure pass-through.
 *
 * Caller discipline (not enforced by the compiler): this must be
 * called AFTER the worker thread has released s_audioRenderLock, never while
 * still holding it. NativeAudio_XAStreamPump() internally takes
 * NativeAudio_LockOutput()/UnlockOutput(), which resolve to the same
 * s_audioRenderLock via the SDL_LockAudioStream/UnlockAudioStream shim.
 * LightLock is non-recursive, so calling this from inside
 * NativeAudio_RenderVBlankForAudioThread's own lock/unlock bracket (or any
 * other span still holding that lock) would make the worker thread
 * self-deadlock trying to re-lock a lock it already holds -- silent at
 * compile time, visible only as a hang at runtime. The worker loop calls
 * this as its own separate, sequential step after the render lock is
 * released, not nested inside it.
 */
void NativeAudio_XAPumpForAudioThread(void)
{
	NativeAudio_XAStreamPump();
}
#endif

int NativeAudio_RenderFrames(s16 *out, int frameCount)
{
	int framesRendered;

	if ((out == NULL) || (frameCount <= 0))
	{
		return 0;
	}

	NativeAudio_LockOutput();

	framesRendered = NativeAudio_RenderFramesNoLock(out, frameCount);

	NativeAudio_UnlockOutput();

	return framesRendered;
}

#if defined(CTR_INTERNAL)
/*
 * ===================================================== mixer golden test ===
 *
 * Regression harness for changing NativeAudio_MixFrame. The mixer is a pure
 * function of SPU state, so this does not need the GAME to be deterministic --
 * only the mixer's INPUT. That matters, because on PC the game is not frame
 * -deterministic without the replay scheduler: Native_CatchUpDueVBlanks emits
 * vblanks off the wall clock, so two runs diverge.
 *
 * Instead: capture one real SPU state to a file, then replay it through the
 * mixer as often as you like. struct NativeAudioSnapshot carries the whole
 * 512KB spuSampleMem alongside every voice, the reverb state and XA, and
 * CopyStateToVoice deliberately restores each active voice with pcm=NULL and
 * sampleDirty=1, so the decoded PCM is rebuilt from the captured sample RAM.
 * The blob is therefore self-contained and bit-reproducible.
 *
 * Why a checksum and not ears: the per-voice-across-a-block restructure is
 * supposed to produce IDENTICAL samples, and the mix accumulates in ints, so
 * there is no rounding drift to excuse a difference. Bit-identical is the
 * correct bar, and it catches things listening never would -- in particular
 * NativeAudio_ReclaimVoicePcmArenaIfIdleNoLock, which runs per sample when a
 * voice stops and can free voice->pcm, making any reordering order-sensitive.
 *
 *   Capture:  ctr_native --audio-capture out.spustate [--audio-capture-vblank N]
 *   Compare:  ctr_native --audio-golden out.spustate [--audio-golden-raw out.raw]
 *
 * Capture at a vblank where the mixer is busy -- check the reverb and voice
 * counts are non-zero, or the test proves nothing. Reverb especially: an early
 * measurement in this project recorded "reverb is never configured" because
 * every sample was taken before the game reached content that enables it.
 */
#define NATIVE_AUDIO_GOLDEN_PATH_MAX    512
#define NATIVE_AUDIO_GOLDEN_VBLANKS     600 /* 10 s of mixer work at 60Hz */

global_variable char s_goldenCapturePath[NATIVE_AUDIO_GOLDEN_PATH_MAX];
global_variable int s_goldenCaptureAtVblank = -1;
global_variable int s_goldenCaptureVblanks;

internal u32 NativeAudio_GoldenHash(u32 hash, const void *data, int bytes)
{
	const u8 *cursor = (const u8 *)data;

	/* FNV-1a, 32-bit. Any order-sensitive change moves it. */
	for (int i = 0; i < bytes; i++)
	{
		hash ^= (u32)cursor[i];
		hash *= 16777619u;
	}

	return hash;
}

internal int NativeAudio_GoldenCaptureNow(const char *path)
{
	const int stateSize = NativeAudio_GetStateSize();
	void *blob = malloc((size_t)stateSize);
	FILE *file;
	int liveVoices = 0;
	int written;

	if (blob == NULL)
	{
		return 0;
	}

	if (!NativeAudio_CaptureState(blob, stateSize))
	{
		free(blob);
		return 0;
	}

	file = fopen(path, "wb");
	if (file == NULL)
	{
		Platform_LogError("[CTR AudioGolden] cannot write %s\n", path);
		free(blob);
		return 0;
	}

	written = (int)fwrite(blob, 1, (size_t)stateSize, file);
	fclose(file);
	free(blob);

	for (int i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		if (s_audio.voices[i].active && (s_audio.voices[i].adsrPhase != NATIVE_AUDIO_ADSR_OFF))
		{
			liveVoices++;
		}
	}

	Platform_Log("[CTR AudioGolden] captured %d bytes to %s (liveVoices=%d reverbSamples=%d xaActive=%d)\n", written, path, liveVoices,
	             s_audio.reverb.sizeSamples, (int)s_audio.xa.active);
	if ((liveVoices == 0) && (s_audio.reverb.sizeSamples == 0))
	{
		Platform_LogWarn("[CTR AudioGolden] WARNING: no live voices and no reverb -- this state exercises almost nothing.\n");
	}

	return written == stateSize;
}

void NativeAudio_GoldenCaptureTick(void)
{
	if (s_goldenCaptureAtVblank < 0)
	{
		return;
	}

	s_goldenCaptureVblanks++;
	if (s_goldenCaptureVblanks < s_goldenCaptureAtVblank)
	{
		return;
	}

	s_goldenCaptureAtVblank = -1;
	NativeAudio_GoldenCaptureNow(s_goldenCapturePath);
	Platform_LogFlush();
	exit(0);
}

internal int NativeAudio_GoldenRun(const char *statePath, const char *rawPath)
{
	s16 block[NATIVE_AUDIO_VBLANK_FRAMES * NATIVE_AUDIO_CHANNELS];
	const int stateSize = NativeAudio_GetStateSize();
	void *blob = malloc((size_t)stateSize);
	FILE *file;
	FILE *rawFile = NULL;
	u32 hash = 2166136261u;
	long totalFrames = 0;
	int read;

	if (blob == NULL)
	{
		return 1;
	}

	/*
	 * Render on the deterministic clock, exactly as the 3DS does, so this
	 * measures the same code path the port actually runs.
	 *
	 * This MUST happen before RestoreState, not after it. Platform_Init has
	 * already opened and resumed the output device by the time --audio-golden
	 * is handled (main.c), and outside deterministic mode the device thread's
	 * callback mixes directly via NativeAudio_RenderFramesNoLock. Flipping
	 * after the restore lets a callback race the restored snapshot, making the
	 * golden hash non-deterministic. In deterministic mode the same callback
	 * only drains already-rendered PCM and cannot touch mixer state, so
	 * ordering alone closes it.
	 */
	NativeAudio_SetDeterministicRenderMode(1);

	file = fopen(statePath, "rb");
	if (file == NULL)
	{
		Platform_LogError("[CTR AudioGolden] cannot read %s\n", statePath);
		free(blob);
		return 1;
	}

	read = (int)fread(blob, 1, (size_t)stateSize, file);
	fclose(file);

	if (read != stateSize)
	{
		Platform_LogError("[CTR AudioGolden] %s is %d bytes, expected %d (rebuild changed the snapshot layout?)\n", statePath, read, stateSize);
		free(blob);
		return 1;
	}

	if (!NativeAudio_RestoreState(blob, stateSize))
	{
		Platform_LogError("[CTR AudioGolden] RestoreState rejected %s\n", statePath);
		free(blob);
		return 1;
	}
	free(blob);

	if (rawPath != NULL)
	{
		rawFile = fopen(rawPath, "wb");
		if (rawFile == NULL)
		{
			Platform_LogWarn("[CTR AudioGolden] cannot write %s, continuing with checksum only\n", rawPath);
		}
	}

	for (int vblank = 0; vblank < NATIVE_AUDIO_GOLDEN_VBLANKS; vblank++)
	{
		const int frames = NativeAudio_RenderFrames(block, NATIVE_AUDIO_VBLANK_FRAMES);
		const int bytes = frames * NATIVE_AUDIO_CHANNELS * (int)sizeof(s16);

		if (frames <= 0)
		{
			break;
		}

		hash = NativeAudio_GoldenHash(hash, block, bytes);
		totalFrames += frames;

		if (rawFile != NULL)
		{
			fwrite(block, 1, (size_t)bytes, rawFile);
		}
	}

	if (rawFile != NULL)
	{
		fclose(rawFile);
	}

	Platform_Log("[CTR AudioGolden] state=%s frames=%ld checksum=%08x\n", statePath, totalFrames, hash);
	Platform_LogFlush();
	return 0;
}

/*
 * ================================================ lifecycle self-test ======
 *
 * The 3DS lifecycle fast path (NativeAudio_LifecycleVBlankNoLock) claims to
 * leave voice state exactly where the mixer's per-sample loop would. That is
 * a claim about the most correctness-critical code in the audio system, made
 * for a platform whose only acceptance channel is a human with a console --
 * so it gets checked here, on PC, against the real thing.
 *
 * The comparison is deliberately NOT against a hand-written reference copy
 * of the per-sample loop (a transcription bug would then pass): it runs
 * NativeAudio_RenderFramesNoLock -- the actual mixer, MixFrame and all --
 * over a vblank of randomized voice states, then re-runs the same seed
 * through the fast path and diffs every field the game can observe. On PC
 * MixFrame additionally generates samples and processes reverb; neither
 * writes voice state, so the two are directly comparable.
 *
 * The batching itself is exercised the same way twice over: maxBatch=1
 * degenerates the fast path to one sample at a time, so a maxBatch=1 vs
 * maxBatch=735 diff isolates the event-skipping arithmetic from everything
 * else. Both diffs must be clean.
 *
 * Run: build/ctr_native --audio-lifecycle-selftest   (exits without booting)
 */
internal u32 NativeAudio_SelfTestRandom(u32 *state)
{
	*state = (*state * 1664525u) + 1013904223u;
	return *state;
}

internal void NativeAudio_SelfTestSeedVoice(struct NativeAudioVoice *voice, s16 *pcm, int pcmCount, u32 *rng)
{
	static const int phases[] = {NATIVE_AUDIO_ADSR_ATTACK, NATIVE_AUDIO_ADSR_DECAY, NATIVE_AUDIO_ADSR_SUSTAIN, NATIVE_AUDIO_ADSR_RELEASE};
	static const int modes[] = {SPU_VOICE_LINEARIncN, SPU_VOICE_LINEARIncR, SPU_VOICE_LINEARDecN, SPU_VOICE_LINEARDecR,
	                            SPU_VOICE_EXPIncN,    SPU_VOICE_EXPIncR,    SPU_VOICE_EXPDec};
	u32 r;

	memset(voice, 0, sizeof(*voice));

	voice->pcm = pcm;
	voice->sampleCount = 64 + (int)(NativeAudio_SelfTestRandom(rng) % (u32)(pcmCount - 80));
	voice->active = 1;
	voice->sampleDirty = 0;

	/*
	 * Loop points sometimes deliberately out of order or past the decoded
	 * extent: the scan produces those for malformed SPU data, and they are
	 * exactly the cases where the fast path's position-event clamp has to
	 * agree with the per-sample checks rather than reason about them.
	 */
	voice->loopEnabled = (NativeAudio_SelfTestRandom(rng) & 3u) != 0;
	voice->loopStart = (int)(NativeAudio_SelfTestRandom(rng) % (u32)voice->sampleCount);
	voice->loopEnd = (int)(NativeAudio_SelfTestRandom(rng) % (u32)(voice->sampleCount + 16));

	voice->positionFp = ((u64)(NativeAudio_SelfTestRandom(rng) % (u32)voice->sampleCount) << NATIVE_AUDIO_FP_SHIFT) |
	                    (NativeAudio_SelfTestRandom(rng) & 0xffffu);

	voice->adsrPhase = (u8)phases[NativeAudio_SelfTestRandom(rng) % 4u];
	voice->adsrLevel = (s32)(NativeAudio_SelfTestRandom(rng) % 0x8000u);
	voice->adsrCounter = NativeAudio_SelfTestRandom(rng) % NATIVE_AUDIO_ADSR_STEP_BIT;

	/* Rates span the whole range on purpose: shift <= 11 overflows the
	 * counter every sample (no batching possible), 0x1f is one overflow per
	 * 32,768 samples (the case the fast path exists for), and all-ones is
	 * the frozen-phase early-out. */
	voice->attr.ar = (u16)(NativeAudio_SelfTestRandom(rng) & 0x7fu);
	voice->attr.dr = (u16)(NativeAudio_SelfTestRandom(rng) & 0x0fu);
	voice->attr.sr = (u16)(NativeAudio_SelfTestRandom(rng) & 0x7fu);
	voice->attr.rr = (u16)(NativeAudio_SelfTestRandom(rng) & 0x1fu);
	voice->attr.sl = (u16)(NativeAudio_SelfTestRandom(rng) & 0x0fu);
	voice->attr.a_mode = modes[NativeAudio_SelfTestRandom(rng) % 7u];
	voice->attr.s_mode = modes[NativeAudio_SelfTestRandom(rng) % 7u];
	voice->attr.r_mode = modes[NativeAudio_SelfTestRandom(rng) % 7u];

	r = NativeAudio_SelfTestRandom(rng);
	voice->attr.pitch = ((r & 7u) == 0) ? 0 : (u16)(r % 0x3fffu); /* pitch 0 is its own branch */
	voice->attr.volume.left = 0x3fff;
	voice->attr.volume.right = 0x3fff;
	voice->reverb = 0;
}

internal int NativeAudio_SelfTestVoiceDiffers(const struct NativeAudioVoice *a, const struct NativeAudioVoice *b)
{
	return (a->positionFp != b->positionFp) || (a->adsrLevel != b->adsrLevel) || (a->adsrCounter != b->adsrCounter) || (a->adsrPhase != b->adsrPhase) ||
	       (a->active != b->active) || (a->looped != b->looped) || (a->attr.envx != b->attr.envx) || (a->sampleCount != b->sampleCount) ||
	       (a->loopStart != b->loopStart) || (a->loopEnd != b->loopEnd) || (a->loopEnabled != b->loopEnabled);
}

int NativeAudio_LifecycleSelfTest(void)
{
	enum
	{
		SELFTEST_CASES = 3000,
		SELFTEST_PCM = 2048
	};
	static s16 pcm[SELFTEST_PCM];
	static struct NativeAudioVoice seed[NATIVE_AUDIO_SPU_VOICE_COUNT];
	static struct NativeAudioVoice fromMixer[NATIVE_AUDIO_SPU_VOICE_COUNT];
	static struct NativeAudioVoice fromStepped[NATIVE_AUDIO_SPU_VOICE_COUNT];
	static s16 scratch[NATIVE_AUDIO_VBLANK_FRAMES * NATIVE_AUDIO_CHANNELS];
	u32 rng = 0x13579bdfu;
	int mixerMismatches = 0;
	int batchMismatches = 0;
	int caseIndex;
	int i;

	for (i = 0; i < SELFTEST_PCM; i++)
	{
		pcm[i] = (s16)((NativeAudio_SelfTestRandom(&rng) & 0xffffu) - 0x8000);
	}

	/* Minimal mixer state: initialized, unmuted, no XA, no reverb -- the
	 * 3DS worker path's own conditions, so MixFrame's voice handling is the
	 * only thing that moves. */
	memset(&s_audio, 0, sizeof(s_audio));
	s_audio.init = 1;
	s_audio.muted = 0;
	s_audio.masterVolumeLeft = 0x3fff;
	s_audio.masterVolumeRight = 0x3fff;
	/* Set directly rather than via the setter: the memset above just cleared
	 * it, and this keeps SDL's callback thread out of the mixer (it drains
	 * only, in this mode) so it cannot advance the voices being compared. */
	s_audio.output.deterministicRenderMode = 1;

	for (caseIndex = 0; caseIndex < SELFTEST_CASES; caseIndex++)
	{
		for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
		{
			NativeAudio_SelfTestSeedVoice(&seed[i], pcm, SELFTEST_PCM, &rng);
		}

		/* Reference: the real mixer over one vblank. */
		memcpy(s_audio.voices, seed, sizeof(seed));
		NativeAudio_RenderFramesNoLock(scratch, NATIVE_AUDIO_VBLANK_FRAMES);
		memcpy(fromMixer, s_audio.voices, sizeof(fromMixer));

		/* Fast path, one sample at a time -- isolates transcription from
		 * batching. */
		memcpy(s_audio.voices, seed, sizeof(seed));
		NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, 1);
		memcpy(fromStepped, s_audio.voices, sizeof(fromStepped));

		/* Fast path, batching the whole vblank -- what 3DS actually runs. */
		memcpy(s_audio.voices, seed, sizeof(seed));
		NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, NATIVE_AUDIO_VBLANK_FRAMES);

		for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
		{
			if (NativeAudio_SelfTestVoiceDiffers(&fromMixer[i], &fromStepped[i]))
			{
				if (mixerMismatches < 4)
				{
					Platform_Log("[CTR AudioLifecycle] case %d voice %d: stepped != mixer (phase %d/%d level %d/%d pos %llu/%llu)\n", caseIndex, i,
					             fromStepped[i].adsrPhase, fromMixer[i].adsrPhase, (int)fromStepped[i].adsrLevel, (int)fromMixer[i].adsrLevel,
					             (unsigned long long)fromStepped[i].positionFp, (unsigned long long)fromMixer[i].positionFp);
				}
				mixerMismatches++;
			}
			if (NativeAudio_SelfTestVoiceDiffers(&s_audio.voices[i], &fromStepped[i]))
			{
				if (batchMismatches < 4)
				{
					Platform_Log("[CTR AudioLifecycle] case %d voice %d: batched != stepped (phase %d/%d level %d/%d pos %llu/%llu)\n", caseIndex, i,
					             s_audio.voices[i].adsrPhase, fromStepped[i].adsrPhase, (int)s_audio.voices[i].adsrLevel, (int)fromStepped[i].adsrLevel,
					             (unsigned long long)s_audio.voices[i].positionFp, (unsigned long long)fromStepped[i].positionFp);
				}
				batchMismatches++;
			}
		}
	}

	Platform_Log("[CTR AudioLifecycle] %d cases x %d voices x %d frames: mixer-vs-stepped %d mismatches, stepped-vs-batched %d mismatches -- %s\n",
	             SELFTEST_CASES, NATIVE_AUDIO_SPU_VOICE_COUNT, NATIVE_AUDIO_VBLANK_FRAMES, mixerMismatches, batchMismatches,
	             ((mixerMismatches == 0) && (batchMismatches == 0)) ? "PASS" : "FAIL");

	return ((mixerMismatches == 0) && (batchMismatches == 0)) ? 0 : 1;
}

/*
 * The same equivalence check plus a cost ratio, run against a REAL captured
 * SPU state instead of randomized voices (debug/audio/*.spustate, produced
 * by --audio-capture; see docs/REPLAYS.md). Randomized envelopes exercise
 * the arithmetic but say nothing about how often CTR's actual voices sit at
 * rates worth skipping, and this project has a history of measuring content
 * that did not exercise the thing being measured. The ratio here is still
 * only indicative -- x86 is not the ARM11, and one capture is not every
 * track -- so it is a sanity bound, not the answer. The answer is `lifeUs`
 * on the next hardware log.
 *
 * Run: build/ctr_native --audio-lifecycle-bench debug/audio/mixer.spustate
 */
int NativeAudio_LifecycleBench(const char *statePath)
{
	enum
	{
		BENCH_REPEATS = 20,
		BENCH_VBLANKS = 300
	};
	const int stateSize = NativeAudio_GetStateSize();
	void *blob = malloc((size_t)stateSize);
	static struct NativeAudioVoice stepped[NATIVE_AUDIO_SPU_VOICE_COUNT];
	FILE *file;
	clock_t steppedClocks = 0;
	clock_t batchedClocks = 0;
	int mismatches = 0;
	int repeat;
	int vblank;
	int i;

	if (blob == NULL)
	{
		return 1;
	}

	file = fopen(statePath, "rb");
	if ((file == NULL) || ((int)fread(blob, 1, (size_t)stateSize, file) != stateSize))
	{
		Platform_LogError("[CTR AudioLifecycle] cannot read %s (expected %d bytes)\n", statePath, stateSize);
		if (file != NULL)
		{
			fclose(file);
		}
		free(blob);
		return 1;
	}
	fclose(file);

	/*
	 * The audio device is open and its callback thread mixes s_audio -- without
	 * deterministic mode it will advance these very voices underneath the
	 * comparison, turning a clean equivalence check into noise. Deterministic
	 * mode is how --audio-golden already keeps the callback off the mixer: in
	 * that mode SDL only drains already-queued PCM and never calls the mixer
	 * itself.
	 */
	NativeAudio_SetDeterministicRenderMode(1);

	for (repeat = 0; repeat < BENCH_REPEATS; repeat++)
	{
		clock_t start;

		if (!NativeAudio_RestoreState(blob, stateSize))
		{
			Platform_LogError("[CTR AudioLifecycle] RestoreState rejected %s\n", statePath);
			free(blob);
			return 1;
		}
		start = clock();
		for (vblank = 0; vblank < BENCH_VBLANKS; vblank++)
		{
			NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, 1);
		}
		steppedClocks += clock() - start;
		memcpy(stepped, s_audio.voices, sizeof(stepped));

		if (!NativeAudio_RestoreState(blob, stateSize))
		{
			free(blob);
			return 1;
		}
		start = clock();
		for (vblank = 0; vblank < BENCH_VBLANKS; vblank++)
		{
			NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, NATIVE_AUDIO_VBLANK_FRAMES);
		}
		batchedClocks += clock() - start;

		for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
		{
			if (NativeAudio_SelfTestVoiceDiffers(&s_audio.voices[i], &stepped[i]))
			{
				mismatches++;
			}
		}
	}

	/*
	 * Any mismatch above is a divergence somewhere in 300 vblanks of
	 * compounding state -- useless on its own. Re-run the two paths in
	 * lockstep on a full mixer snapshot, comparing after EVERY vblank, so
	 * the first one that differs is named along with the voice and field.
	 */
	if (mismatches != 0)
	{
		static struct NativeAudioState snapshot;
		static struct NativeAudioVoice beforeVblank[NATIVE_AUDIO_SPU_VOICE_COUNT];
		int reported = 0;

		/* blob is still the loaded snapshot here -- the outer free is below. */
		NativeAudio_RestoreState(blob, stateSize);

		for (vblank = 0; (vblank < BENCH_VBLANKS) && (reported < 3); vblank++)
		{
			unsigned long steppedIters;
			unsigned long steppedFrames;

			memcpy(beforeVblank, s_audio.voices, sizeof(beforeVblank));

			s_lifecycleDebugIters = 0;
			s_lifecycleDebugFrames = 0;
			NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, 1);
			steppedIters = s_lifecycleDebugIters;
			steppedFrames = s_lifecycleDebugFrames;
			memcpy(&snapshot, &s_audio, sizeof(snapshot));
			memcpy(stepped, s_audio.voices, sizeof(stepped));

			memcpy(s_audio.voices, beforeVblank, sizeof(beforeVblank));
			s_lifecycleDebugIters = 0;
			s_lifecycleDebugFrames = 0;
			NativeAudio_LifecycleVBlankNoLock(NATIVE_AUDIO_VBLANK_FRAMES, NATIVE_AUDIO_VBLANK_FRAMES);

			for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
			{
				if (NativeAudio_SelfTestVoiceDiffers(&s_audio.voices[i], &stepped[i]) && (reported < 3))
				{
					const struct NativeAudioVoice *in = &beforeVblank[i];

					Platform_Log("[CTR AudioLifecycle] first divergence vblank %d voice %d\n", vblank, i);
					Platform_Log("  accounting: stepped %lu iters / %lu frames, batched %lu iters / %lu frames (expect %d frames each)\n", steppedIters,
					             steppedFrames, s_lifecycleDebugIters, s_lifecycleDebugFrames, NATIVE_AUDIO_VBLANK_FRAMES * NATIVE_AUDIO_SPU_VOICE_COUNT);
					/* Positions as sample+fraction, not %llu: mingw's stdio
					 * cannot be trusted with 64-bit conversions here, and a
					 * mis-printed number would send this diagnosis somewhere
					 * imaginary. */
					Platform_Log("  in:      phase %d level %d counter %u pos %u+%u pitch %u count %d loop %d [%d,%d) dirty %d pcm %d\n", in->adsrPhase,
					             (int)in->adsrLevel, (unsigned)in->adsrCounter, (unsigned)(in->positionFp >> NATIVE_AUDIO_FP_SHIFT),
					             (unsigned)(in->positionFp & 0xffffu), (unsigned)in->attr.pitch, in->sampleCount, (int)in->loopEnabled, in->loopStart,
					             in->loopEnd, (int)in->sampleDirty, in->pcm != NULL);
					Platform_Log("  ar/dr/sr/rr/sl %u/%u/%u/%u/%u modes %d/%d/%d\n", (unsigned)in->attr.ar, (unsigned)in->attr.dr, (unsigned)in->attr.sr,
					             (unsigned)in->attr.rr, (unsigned)in->attr.sl, in->attr.a_mode, in->attr.s_mode, in->attr.r_mode);
					Platform_Log("  stepped: phase %d level %d counter %u pos %u+%u active %d looped %d count %d\n", stepped[i].adsrPhase,
					             (int)stepped[i].adsrLevel, (unsigned)stepped[i].adsrCounter, (unsigned)(stepped[i].positionFp >> NATIVE_AUDIO_FP_SHIFT),
					             (unsigned)(stepped[i].positionFp & 0xffffu), (int)stepped[i].active, (int)stepped[i].looped, stepped[i].sampleCount);
					Platform_Log("  batched: phase %d level %d counter %u pos %u+%u active %d looped %d count %d\n", s_audio.voices[i].adsrPhase,
					             (int)s_audio.voices[i].adsrLevel, (unsigned)s_audio.voices[i].adsrCounter,
					             (unsigned)(s_audio.voices[i].positionFp >> NATIVE_AUDIO_FP_SHIFT), (unsigned)(s_audio.voices[i].positionFp & 0xffffu),
					             (int)s_audio.voices[i].active, (int)s_audio.voices[i].looped, s_audio.voices[i].sampleCount);
					reported++;
				}
			}

			/* Continue from the stepped path's state so later vblanks are
			 * compared from a known-good baseline rather than compounding. */
			memcpy(&s_audio, &snapshot, sizeof(s_audio));
		}
	}

	free(blob);

	{
		const double steppedMs = (1000.0 * (double)steppedClocks) / (double)CLOCKS_PER_SEC;
		const double batchedMs = (1000.0 * (double)batchedClocks) / (double)CLOCKS_PER_SEC;
		const int vblanksRun = BENCH_REPEATS * BENCH_VBLANKS;

		Platform_Log("[CTR AudioLifecycle] %s: %d vblanks -- per-sample %.3f ms (%.1f us/vblank), batched %.3f ms (%.1f us/vblank), %.2fx, state mismatches %d\n",
		             statePath, vblanksRun, steppedMs, (1000.0 * steppedMs) / (double)vblanksRun, batchedMs, (1000.0 * batchedMs) / (double)vblanksRun,
		             (batchedMs > 0.0) ? (steppedMs / batchedMs) : 0.0, mismatches);
	}

	return (mismatches == 0) ? 0 : 1;
}

int NativeAudio_GoldenConfigureFromArgs(int argc, char **argv)
{
	const char *capturePath = NULL;
	const char *captureVblank = NULL;
	const char *goldenPath = NULL;
	const char *rawPath = NULL;

	/* Own scan: this flag takes no value, so the paired-argument loop below
	 * would skip it whenever it lands last on the command line. */
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--audio-lifecycle-selftest") == 0)
		{
			NativeAudio_LifecycleSelfTest();
			return 1; /* nonzero: main() exits without entering the game */
		}
	}

	for (int i = 1; i < argc; i++)
	{
		if ((i + 1) >= argc)
		{
			break;
		}

		if (strcmp(argv[i], "--audio-capture") == 0)
		{
			capturePath = argv[i + 1];
		}
		else if (strcmp(argv[i], "--audio-capture-vblank") == 0)
		{
			captureVblank = argv[i + 1];
		}
		else if (strcmp(argv[i], "--audio-golden") == 0)
		{
			goldenPath = argv[i + 1];
		}
		else if (strcmp(argv[i], "--audio-golden-raw") == 0)
		{
			rawPath = argv[i + 1];
		}
		else if (strcmp(argv[i], "--audio-lifecycle-bench") == 0)
		{
			NativeAudio_LifecycleBench(argv[i + 1]);
			return 1; /* nonzero: main() exits without entering the game */
		}
	}

	if (goldenPath != NULL)
	{
		/* Returns nonzero so main() exits without ever entering the game. */
		NativeAudio_GoldenRun(goldenPath, rawPath);
		return 1;
	}

	if (capturePath != NULL)
	{
		snprintf(s_goldenCapturePath, sizeof(s_goldenCapturePath), "%s", capturePath);
		s_goldenCaptureAtVblank = (captureVblank != NULL) ? atoi(captureVblank) : 1800;
		if (s_goldenCaptureAtVblank < 1)
		{
			s_goldenCaptureAtVblank = 1;
		}
		Platform_Log("[CTR AudioGolden] will capture SPU state to %s at vblank %d\n", s_goldenCapturePath, s_goldenCaptureAtVblank);
	}

	return 0;
}
#endif /* CTR_INTERNAL */

#if defined(__3DS__)
/*
 * Read by the 3DS stats block in native_renderer_3ds.c. Same translation unit
 * (main.c includes native_audio.c before native_renderer_3ds.c), so these stay
 * file-scope rather than growing the platform header for a diagnostic.
 */
global_variable unsigned long s_audioShapeVoiceAccum;
global_variable unsigned long s_audioShapeVblanks;
global_variable int s_audioShapeReverbSamples;
global_variable int s_audioShapeXaActive;
#endif

void NativeAudio_StepVBlank(void)
{
#ifdef CTR_INTERNAL
	int shouldReportStats = 0;
	int underrunFrames = 0;
	int overflowFrames = 0;
	int queuedFrames = 0;
#endif

	if (!s_audio.init)
	{
		return;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_AUDIO_VBLANK);

#if defined(__3DS__)
	/*
	 * On 3DS, StepVBlank does NOT take the big render lock at all -- the
	 * audio-thread handshake below uses its own small counter lock
	 * internally (N3DS_AudioThreadNotifyVBlank), and the shape-stats block
	 * here is an explicitly unlocked, best-effort read, matching the
	 * existing precedent for exactly this class of diagnostic
	 * (NativeAudio_XAStreamSectorsBuffered: reads without a lock, a
	 * best-effort periodic counter, not a correctness-sensitive read).
	 *
	 * Shape of the mixer's workload, sampled once per vblank rather than per
	 * sample so it costs nothing measurable. NativeAudio_MixFrame runs the
	 * 24-voice loop AND the reverb chain for every one of the 735 sample
	 * frames in a vblank, so "how many voices are actually live" and "is the
	 * reverb engine configured at all" together decide most of that cost.
	 */
	{
		int liveVoices = 0;

		for (int v = 0; v < NATIVE_AUDIO_SPU_VOICE_COUNT; v++)
		{
			if (s_audio.voices[v].active && (s_audio.voices[v].adsrPhase != NATIVE_AUDIO_ADSR_OFF))
			{
				liveVoices++;
			}
		}

		s_audioShapeVoiceAccum += (unsigned long)liveVoices;
		s_audioShapeVblanks++;
		s_audioShapeReverbSamples = s_audio.reverb.sizeSamples;
		s_audioShapeXaActive = s_audio.xa.active;
	}

	if (s_audio.output.deterministicRenderMode)
	{
		if (N3DS_AudioThreadIsRunning())
		{
			N3DS_AudioThreadNotifyVBlank(); /* O(1): small-lock bump + signal only */
		}
		else
		{
			/* No worker thread (an original 3DS has one usable core, or the
			 * thread could not be created): the same DSP path, driven from
			 * here. Voices play on their own DSP channels and XA goes to the
			 * music channel at its source rate, so nothing is mixed or
			 * resampled on this thread. Includes the XA stream pump and the
			 * sample-decode pump. */
			N3DS_AudioInlineStepVBlank();
		}
	}
	/* No CTR_INTERNAL stats-reporting block on this path: with output
	 * discarded on 3DS, underrun/overflow/queued counters are not
	 * meaningful the way they are on PC. audioThreadBacklogVBlanks /
	 * audioThreadDroppedVBlanks (N3DS_AudioThreadGetBacklog/GetDropped) are
	 * the 3DS-relevant numbers instead. shouldReportStats therefore stays
	 * false on this path, so the shared CTR_INTERNAL block below is a no-op
	 * here without needing its own #if. */
#else
	NativeAudio_LockOutput();

	if (s_audio.output.deterministicRenderMode)
	{
		s16 renderedFrames[NATIVE_AUDIO_VBLANK_FRAMES * NATIVE_AUDIO_CHANNELS];

		// NOTE(aalhendi): Replay/TAS mode advances SPU/XA from the native PS1
		// VBlank clock. SDL only drains the scheduled PCM in its callback.
		int framesRendered = NativeAudio_RenderFramesNoLock(renderedFrames, NATIVE_AUDIO_VBLANK_FRAMES);
		NativeAudio_QueueRenderedFramesNoLock(renderedFrames, framesRendered);
	}

#ifdef CTR_INTERNAL
	shouldReportStats = NativeAudio_ShouldReportOutputStatsNoLock(&underrunFrames, &overflowFrames, &queuedFrames);
#endif

	NativeAudio_UnlockOutput();
#endif

#if defined(__3DS__)
	if (!s_audio.output.deterministicRenderMode)
	{
		/* Nothing above serviced this vblank. With deterministic render mode
		 * on, the worker or N3DS_AudioInlineStepVBlank pumps XA itself, on
		 * the same thread that advances the XA position. */
		NativeAudio_XAStreamPump();
	}
#else
	NativeAudio_XAStreamPump();
#endif

	NativePerf_EndScope(NATIVE_PERF_BUCKET_AUDIO_VBLANK);

#if defined(CTR_INTERNAL)
	/* Outside the perf scope: capturing writes ~530KB and then exits. */
	NativeAudio_GoldenCaptureTick();
#if !defined(__3DS__)
	{
		/* Defined later in this unity TU (native_renderer.c, PC-only file);
		 * same vblank cadence as the audio capture above. */
		void NativeRenderer_VramCaptureTick(void);

		NativeRenderer_VramCaptureTick();
		/* Offscreen-split capture shares this vblank timebase so its
		 * inventory lines correlate with --vram-capture-vblank numbers. */
		NativeOffscreenCapture_Tick();
	}
#endif
#endif

#if defined(__3DS__) && defined(CTR_INTERNAL)
	/*
	 * Pop receipt, on its own ~2s throttle so it prints even when there is no
	 * source underrun. The stats block below is gated on underrun/overflow being
	 * nonzero (NativeAudio_ShouldReportOutputStatsNoLock) and stays silent in the
	 * pure seam-pop case, which is exactly the case we need to see. flush=full/early:
	 * aggregation into big wave buffers is what suppresses per-vblank seams, so an
	 * early-dominant ratio means the DSP kept running dry and got short buffers back
	 * -> seam pops. dropped= is pool-full drops. On a hardware sitting: early-flush
	 * high => seam pops (raise cushion/aggregate depth); underrun high => source
	 * starvation (raise SD-side XA read-ahead) -- two different fixes.
	 */
	{
		static int s_naFlushReportCountdown = 0;

		if (s_naFlushReportCountdown > 0)
		{
			s_naFlushReportCountdown--;
		}
		else
		{
			char line[160];
			int fullFlushes = 0, earlyFlushes = 0;
			int droppedBlocks;

			s_naFlushReportCountdown = 120; /* ~2s at 60Hz */
			N3DS_NdspGetFlushCounts(&fullFlushes, &earlyFlushes);
			droppedBlocks = N3DS_NdspGetDroppedBlocks();

			snprintf(line, sizeof(line), "[CTR-3DS] audio flush=%d/%d(full/early) dropped=%d underrun=%d", fullFlushes, earlyFlushes, droppedBlocks,
			         s_audio.output.underrunFrames);
			N3DS_DebugLog(line);
		}
	}
#endif

#ifdef CTR_INTERNAL
	if (shouldReportStats)
	{
#if defined(__3DS__)
		/*
		 * To the log, never stdout: on 3DS stdout IS the bottom-screen console
		 * that the stats block draws with absolute cursor positioning, and an
		 * unpositioned line here scrolls the whole thing out from under it.
		 */
		{
			char line[128];

			snprintf(line, sizeof(line), "[CTR-3DS] audio out underrun=%d overflow=%d queued=%d cbMax=%d", underrunFrames, overflowFrames, queuedFrames,
			         s_audio.output.callbackMaxRequestFrames);
			N3DS_DebugLog(line);
		}
#else
		printf("[CTR 3D] Audio output stats: underrunFrames=%d overflowFrames=%d queuedFrames=%d callbackMaxRequestFrames=%d\n", underrunFrames,
		       overflowFrames, queuedFrames, s_audio.output.callbackMaxRequestFrames);
#endif
	}
#endif
}

internal int NativeAudio_OpenDevice(void)
{
	SDL_AudioSpec want;
	SDL_AudioSpec srcSpec;
	SDL_AudioSpec dstSpec;
	SDL_AudioSpec deviceSpec;
	int deviceSampleFrames = 0;

	if (NativeAudio_OutputOpen())
	{
		return 1;
	}

	NativeAudio_SelectDriverHint();

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
	{
		return 0;
	}

	memset(&want, 0, sizeof(want));
	memset(&srcSpec, 0, sizeof(srcSpec));
	memset(&dstSpec, 0, sizeof(dstSpec));
	memset(&deviceSpec, 0, sizeof(deviceSpec));
	want.freq = NATIVE_AUDIO_SAMPLE_RATE;
	want.format = SDL_AUDIO_S16;
	want.channels = NATIVE_AUDIO_CHANNELS;

	s_audio.output.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NativeAudio_StreamCallback, NULL);
	if (s_audio.output.stream == NULL)
	{
		fprintf(stderr, "[CTR 3D] SDL audio unavailable: %s\n", SDL_GetError());
		return 0;
	}

	s_audio.output.device = SDL_GetAudioStreamDevice(s_audio.output.stream);
	if (!SDL_GetAudioDeviceFormat(s_audio.output.device, &deviceSpec, &deviceSampleFrames))
	{
		deviceSampleFrames = 0;
	}
	if (!SDL_GetAudioStreamFormat(s_audio.output.stream, &srcSpec, &dstSpec))
	{
		fprintf(stderr, "[CTR 3D] SDL audio stream format unavailable: %s\n", SDL_GetError());
		SDL_DestroyAudioStream(s_audio.output.stream);
		s_audio.output.stream = NULL;
		s_audio.output.device = 0;
		return 0;
	}

	if ((srcSpec.freq != want.freq) || (srcSpec.format != want.format) || (srcSpec.channels != want.channels))
	{
		fprintf(stderr, "[CTR 3D] SDL audio rejected fixed PCM contract: got %d Hz format 0x%x channels %d\n", srcSpec.freq, srcSpec.format,
		        srcSpec.channels);
		SDL_DestroyAudioStream(s_audio.output.stream);
		s_audio.output.stream = NULL;
		s_audio.output.device = 0;
		return 0;
	}

	// Through the log sink, not a bare printf: on 3DS a raw printf paints the
	// bottom-screen console with no gate, and this line lands after the first
	// present -- past the point the panel stops being a boot surface.
	Platform_Log("[CTR 3D] SDL audio stream opened: driver=%s src=%d Hz/%d ch dst=%d Hz/%d ch device=%d Hz/%d ch sampleFrames=%d\n",
	             SDL_GetCurrentAudioDriver(), srcSpec.freq, srcSpec.channels, dstSpec.freq, dstSpec.channels, deviceSpec.freq, deviceSpec.channels,
	             deviceSampleFrames);
	NativeAudio_ClearOutputQueueNoLock();
	if (!SDL_ResumeAudioStreamDevice(s_audio.output.stream))
	{
		fprintf(stderr, "[CTR 3D] SDL audio stream resume failed: %s\n", SDL_GetError());
		SDL_DestroyAudioStream(s_audio.output.stream);
		s_audio.output.stream = NULL;
		s_audio.output.device = 0;
		return 0;
	}

	return 1;
}

void NativeAudio_Shutdown(void)
{
#if defined(__3DS__)
	/* Single confirmed teardown chokepoint (HOME/power -> SDL_EVENT_QUIT ->
	 * exit(0) -> atexit -> Platform_Shutdown -> here), and also reachable
	 * via the early init-failure path before N3DS_AudioThreadInit ever ran
	 * -- N3DS_AudioThreadShutdown is null-safe for that case. */
	N3DS_AudioThreadShutdown();
	NativeAudio_PalVoiceShutdown();
#endif

	if (s_audio.output.stream != NULL)
	{
		SDL_DestroyAudioStream(s_audio.output.stream);
		s_audio.output.stream = NULL;
		s_audio.output.device = 0;
	}

	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

s32 NativeAudio_SpuInit(void)
{
	if (!s_audio.init)
	{
		memset(&s_audio.spu, 0, sizeof(s_audio.spu));
		memset(&s_audio.voices, 0, sizeof(s_audio.voices));
		memset(&s_audio.reverb, 0, sizeof(s_audio.reverb));
		s_audio.cdMixEnabled = 1;
		s_audio.masterVolumeLeft = 0x3fff;
		s_audio.masterVolumeRight = 0x3fff;
		s_audio.commonAttr.mvol.left = 0x3fff;
		s_audio.commonAttr.mvol.right = 0x3fff;
		s_audio.commonAttr.cd.mix = 1;
		s_audio.reverbAttr.mode = SPU_REV_MODE_OFF;
	}

	if (!NativeAudio_OpenDevice())
	{
		return 0;
	}

	s_audio.init = 1;

#if defined(__3DS__)
	// The SPU/XA clocks advance from the VBlank clock on 3DS, not from an
	// output-device pull callback: the worker thread (or the StepVBlank
	// fallback) renders exactly one vblank of samples per emitted vblank and
	// pushes it to the ndsp sink, which drains at its own fixed rate. ndsp is
	// a sink here, never a clock -- keeping production on the vblank clock is
	// what lets the boot sequence's "while (XA_State != 0)" wait progress, and
	// matches how the 3DS renderer is paced.
	NativeAudio_SetDeterministicRenderMode(1);

	// 3DS audio subsystem finishing initialization: bring up the ndsp sink
	// and try to spin up the dedicated core-1 mixer thread
	//. Safe to call every time this function
	// runs, including the repeat calls NativeAudio_PlayXATrack/PlayXAFile
	// make on every XA playback request -- N3DS_AudioThreadInit no-ops if a
	// worker is already running, and always initializes the locks
	// N3DS_AudioLock/N3DS_AudioUnlock need for the ~30 existing call sites.
	N3DS_AudioThreadInit();
#endif

	return 1;
}

u32 NativeAudio_SpuSetTransferStartAddr(u32 addr)
{
	u32 result;

	if (addr > NATIVE_AUDIO_SPU_MEMSIZE)
	{
		return 0;
	}

	NativeAudio_LockOutput();

	s_audio.spu.transferOffset = (int)addr;
	result = (addr < 0x1010) ? 0 : 1;

	NativeAudio_UnlockOutput();

	return result;
}

u32 NativeAudio_SpuWrite(const u8 *addr, u32 size)
{
	int wptrOfs;

	NativeAudio_LockOutput();

	wptrOfs = s_audio.spu.transferOffset;
	if ((addr == NULL) || (size == 0) || (size > (u32)NATIVE_AUDIO_SPU_MEMSIZE) || (wptrOfs < 0) || (size > (u32)(NATIVE_AUDIO_SPU_MEMSIZE - wptrOfs)))
	{
		NativeAudio_UnlockOutput();
		return 0;
	}

	memcpy(&s_audio.spu.memory[wptrOfs], addr, size);
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
	s_voiceProf.windowSpuWriteBytes += (int)size;
#endif
#if defined(__3DS__)
	/* Range-precise: refresh playing entries in place (SPU streaming), mark
	 * only actually-overwritten entries stale. The v1 nuke-everything here
	 * thrashed the whole decoded cache dozens of times a second during
	 * streamed speech -- see NativeAudio_HwStreamInvalidateRange. */
	NativeAudio_HwStreamInvalidateRange((u32)wptrOfs, (u32)wptrOfs + size);
#else
	// NOTE(aalhendi): SPU RAM writes invalidate all decoded voice PCM. Drop cached buffers
	// so stale samples do not accumulate in the arena during long sessions.
	NativeAudio_ResetVoicePcmArenaNoLock(1);
#endif

	NativeAudio_UnlockOutput();

	return size;
}

void NativeAudio_SpuSetVoiceAttr(SpuVoiceAttr *psxAttrib)
{
	int i;

	if (!s_audio.init || psxAttrib == NULL)
	{
		return;
	}

	NativeAudio_LockOutput();

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		struct NativeAudioVoice *voice;

		if ((psxAttrib->voice & SPU_VOICECH(i)) == 0)
		{
			continue;
		}

		voice = &s_audio.voices[i];

		if (psxAttrib->mask & SPU_VOICE_WDSA)
		{
			if (voice->attr.addr != psxAttrib->addr)
			{
				voice->sampleDirty = 1;
			}
			voice->attr.addr = psxAttrib->addr;
		}

		if (psxAttrib->mask & SPU_VOICE_LSAX)
		{
			if (voice->attr.loop_addr != psxAttrib->loop_addr)
			{
				voice->sampleDirty = 1;
			}
			voice->attr.loop_addr = psxAttrib->loop_addr;
		}

		if (psxAttrib->mask & SPU_VOICE_VOLL)
		{
			voice->attr.volume.left = psxAttrib->volume.left;
		}
		if (psxAttrib->mask & SPU_VOICE_VOLR)
		{
			voice->attr.volume.right = psxAttrib->volume.right;
		}
		if (psxAttrib->mask & SPU_VOICE_PITCH)
		{
			voice->attr.pitch = psxAttrib->pitch;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_AR)
		{
			voice->attr.ar = psxAttrib->ar;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_DR)
		{
			voice->attr.dr = psxAttrib->dr;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_SR)
		{
			voice->attr.sr = psxAttrib->sr;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_RR)
		{
			voice->attr.rr = psxAttrib->rr;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_SL)
		{
			voice->attr.sl = psxAttrib->sl;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_AMODE)
		{
			voice->attr.a_mode = psxAttrib->a_mode;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_SMODE)
		{
			voice->attr.s_mode = psxAttrib->s_mode;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_RMODE)
		{
			voice->attr.r_mode = psxAttrib->r_mode;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_ADSR1)
		{
			voice->attr.adsr1 = psxAttrib->adsr1;
		}
		if (psxAttrib->mask & SPU_VOICE_ADSR_ADSR2)
		{
			voice->attr.adsr2 = psxAttrib->adsr2;
		}
		NativeAudio_DecodePackedAdsrToFields(voice, (psxAttrib->mask & SPU_VOICE_ADSR_ADSR1) != 0, (psxAttrib->mask & SPU_VOICE_ADSR_ADSR2) != 0);
		if (psxAttrib->mask & (SPU_VOICE_ADSR_AR | SPU_VOICE_ADSR_DR | SPU_VOICE_ADSR_SR | SPU_VOICE_ADSR_RR | SPU_VOICE_ADSR_SL | SPU_VOICE_ADSR_AMODE |
		                       SPU_VOICE_ADSR_SMODE | SPU_VOICE_ADSR_RMODE | SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_ADSR2))
		{
			NativeAudio_UpdatePackedAdsrFromFields(voice);
		}
	}

	NativeAudio_UnlockOutput();
}

void NativeAudio_SpuSetKey(s32 on_off, u32 voice_bit)
{
	int i;

	if (!s_audio.init)
	{
		return;
	}

	NativeAudio_LockOutput();

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		struct NativeAudioVoice *voice;

		if ((voice_bit & SPU_VOICECH(i)) == 0)
		{
			continue;
		}

		voice = &s_audio.voices[i];
		if (on_off && !s_audio.muted)
		{
#if CTR_NATIVE_AUDIO_DECODE_PROFILE
			s_voiceProf.windowKeyOns++;
			s_voiceProf.frameKeyOns++;
#endif
			if (!NativeAudio_VoiceHasSample(voice) || voice->sampleDirty)
			{
				NativePerf_BeginScope(NATIVE_PERF_BUCKET_AUDIO_VOICE_START);
				NativeAudio_UpdateVoiceSample(voice);
				NativePerf_EndScope(NATIVE_PERF_BUCKET_AUDIO_VOICE_START);
			}

			voice->positionFp = 0;
			voice->looped = 0;
			voice->active = NativeAudio_VoiceHasSample(voice);
			if (voice->active)
			{
				NativeAudio_AdsrKeyOn(voice);
#if defined(__3DS__)
				s_hwVoiceShadow[i].keyonPending = 1; /* mirror starts the DSP channel next vblank */
#endif
			}
			else
			{
				NativeAudio_AdsrForceOff(voice);
			}
		}
		else
		{
			if (on_off)
			{
				NativeAudio_AdsrForceOff(voice);
			}
			else
			{
				NativeAudio_AdsrKeyOff(voice);
			}
		}
	}

	NativeAudio_ReclaimVoicePcmArenaIfIdleNoLock();

	NativeAudio_UnlockOutput();
}

s32 NativeAudio_SpuSetReverb(s32 on_off)
{
	int oldState;

	NativeAudio_LockOutput();

	oldState = s_audio.reverbEnabled;
	s_audio.reverbEnabled = on_off != 0;

	NativeAudio_UnlockOutput();

	return oldState;
}

s32 NativeAudio_SpuSetReverbModeParam(SpuReverbAttr *attr)
{
	u32 mask;

	if (attr == NULL)
	{
		return SPU_INVALID_ARGS;
	}

	mask = attr->mask != 0 ? attr->mask : (SPU_REV_MODE | SPU_REV_DEPTHL | SPU_REV_DEPTHR | SPU_REV_DELAYTIME | SPU_REV_FEEDBACK);

	NativeAudio_LockOutput();

	if (mask & SPU_REV_MODE)
	{
		s_audio.reverbAttr.mode = attr->mode;
		if (attr->mode != SPU_REV_MODE_CHECK)
		{
			NativeAudio_ReverbConfigureModeNoLock(attr->mode);
		}
	}
	if (mask & SPU_REV_DEPTHL)
	{
		s_audio.reverbAttr.depth.left = attr->depth.left;
	}
	if (mask & SPU_REV_DEPTHR)
	{
		s_audio.reverbAttr.depth.right = attr->depth.right;
	}
	if (mask & SPU_REV_DELAYTIME)
	{
		s_audio.reverbAttr.delay = attr->delay;
	}
	if (mask & SPU_REV_FEEDBACK)
	{
		s_audio.reverbAttr.feedback = attr->feedback;
	}
	s_audio.reverbAttr.mask |= mask;

	NativeAudio_UnlockOutput();

	return SPU_SUCCESS;
}

void NativeAudio_SpuSetReverbModeDepth(s16 left, s16 right)
{
	NativeAudio_LockOutput();

	s_audio.reverbAttr.depth.left = left;
	s_audio.reverbAttr.depth.right = right;
	s_audio.reverbAttr.mask |= SPU_REV_DEPTHL | SPU_REV_DEPTHR;

	NativeAudio_UnlockOutput();
}

u32 NativeAudio_SpuSetReverbVoice(s32 on_off, u32 voice_bit)
{
	int i;

	NativeAudio_LockOutput();

	if (on_off)
	{
		s_audio.reverbVoiceBits |= voice_bit;
	}
	else
	{
		s_audio.reverbVoiceBits &= ~voice_bit;
	}

	for (i = 0; i < NATIVE_AUDIO_SPU_VOICE_COUNT; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) != 0)
		{
			s_audio.voices[i].reverb = on_off != 0;
		}
	}

	NativeAudio_UnlockOutput();

	return 0;
}

void NativeAudio_SpuSetCommonMasterVolume(s16 left, s16 right)
{
	NativeAudio_LockOutput();

	s_audio.masterVolumeLeft = left;
	s_audio.masterVolumeRight = right;
	s_audio.commonAttr.mvol.left = left;
	s_audio.commonAttr.mvol.right = right;
	s_audio.commonAttr.mask |= SPU_COMMON_MVOLL | SPU_COMMON_MVOLR;

	NativeAudio_UnlockOutput();
}

void NativeAudio_SpuSetCommonCDMix(s32 enabled)
{
	NativeAudio_LockOutput();

	s_audio.cdMixEnabled = enabled != 0;
	s_audio.commonAttr.cd.mix = enabled;
	s_audio.commonAttr.mask |= SPU_COMMON_CDMIX;

	NativeAudio_UnlockOutput();
}

void NativeAudio_SpuSetCommonCDVolume(s16 left, s16 right)
{
	NativeAudio_LockOutput();

	s_audio.xa.volumeLeft = left;
	s_audio.xa.volumeRight = right;
	s_audio.commonAttr.cd.volume.left = left;
	s_audio.commonAttr.cd.volume.right = right;
	s_audio.commonAttr.mask |= SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR;

	NativeAudio_UnlockOutput();
}

void NativeAudio_SpuSetCommonCDReverb(s32 enabled)
{
	NativeAudio_LockOutput();

	s_audio.cdReverbEnabled = enabled != 0;
	s_audio.commonAttr.cd.reverb = enabled;
	s_audio.commonAttr.mask |= SPU_COMMON_CDREV;

	NativeAudio_UnlockOutput();
}

int NativeAudio_GetXATrackLength(int categoryID, int xaID)
{
	struct NativeAudioXaTrackInfo info;

	if (!NativeAudio_LookupXATrackInfo(categoryID, xaID, &info))
	{
		return 0;
	}

	return info.numSectors;
}

void NativeAudio_StopXA(void)
{
	struct NativeAudioXaClosedSource closed;

	NativeAudio_LockOutput();

	closed = NativeAudio_FreeXA();

	NativeAudio_UnlockOutput();

	if (closed.open)
	{
		NativeAudio_XASourceClose(&closed.source);
	}
}

void NativeAudio_SetXAVolume(int volumeLeft, int volumeRight)
{
	NativeAudio_LockOutput();

	s_audio.xa.volumeLeft = (s16)volumeLeft;
	s_audio.xa.volumeRight = (s16)volumeRight;
	s_audio.commonAttr.cd.volume.left = (s16)volumeLeft;
	s_audio.commonAttr.cd.volume.right = (s16)volumeRight;
	s_audio.commonAttr.mask |= SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR;

	NativeAudio_UnlockOutput();
}

int NativeAudio_IsXAPlaying(void)
{
	int playing;

	NativeAudio_LockOutput();

	playing = s_audio.xa.active;

	NativeAudio_UnlockOutput();

	return playing;
}

int NativeAudio_PlayXATrack(int categoryID, int xaID, int volumeLeft, int volumeRight)
{
	struct NativeAudioXaClosedSource closed;
	int sampleRate;
	int frameCount;

	if (!NativeAudio_SpuInit())
	{
		return 0;
	}

#if defined(__3DS__)
	if (categoryID == NATIVE_AUDIO_XA_CATEGORY_MUSIC)
	{
		/* MUSIC plays are rare (stings, final lap, results) and each one is a
		 * moment the player notices, so log the outcome of every one: whether
		 * the open succeeded, the length it resolved to, and whether it
		 * replaced a track that was still running. */
		int prevActive = NativeAudio_IsXAPlaying();
		int ok = NativeAudio_XAStreamOpenTrack(&s_audio.xaPendingStream, categoryID, xaID, &frameCount, &sampleRate);

		NativeAudio_XAVoiceCacheLog("[XAPLAY] music xa=%d ok=%d frames=%d rate=%d prevActive=%d", xaID, ok, frameCount, sampleRate, prevActive);
		if (!ok)
		{
			return 0;
		}
	}
	else
#endif
	if (!NativeAudio_XAStreamOpenTrack(&s_audio.xaPendingStream, categoryID, xaID, &frameCount, &sampleRate))
	{
		return 0;
	}

	NativeAudio_LockOutput();

	closed = NativeAudio_FreeXA();
	NativeAudio_XAStreamSwap(&s_audio.xaStream, &s_audio.xaPendingStream);
	s_audio.xa.frameCount = frameCount;
	s_audio.xa.sampleRate = sampleRate;
	s_audio.xa.categoryID = categoryID;
	s_audio.xa.xaID = xaID;
	s_audio.xa.hasTrackIdentity = 1;
	s_audio.xa.positionFp = 0;
	s_audio.xa.outputFrame = 0;
	s_audio.xa.stepFp = (u32)(((u64)sampleRate << NATIVE_AUDIO_FP_SHIFT) / NATIVE_AUDIO_SAMPLE_RATE);
	s_audio.xa.volumeLeft = (s16)volumeLeft;
	s_audio.xa.volumeRight = (s16)volumeRight;
	s_audio.commonAttr.cd.volume.left = (s16)volumeLeft;
	s_audio.commonAttr.cd.volume.right = (s16)volumeRight;
	s_audio.commonAttr.mask |= SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR;
	s_audio.xa.active = 1;

	NativeAudio_UnlockOutput();

	if (closed.open)
	{
		NativeAudio_XASourceClose(&closed.source);
	}

	return 1;
}

int NativeAudio_PlayXAFile(const char *relativePath, int channelFilter, int volumeLeft, int volumeRight)
{
	struct NativeAudioXaClosedSource closed;
	int sampleRate;
	int frameCount;

	if (!NativeAudio_SpuInit())
	{
		return 0;
	}

	if (!NativeAudio_XAStreamOpenFile(&s_audio.xaPendingStream, relativePath, channelFilter, &frameCount, &sampleRate))
	{
		return 0;
	}

	NativeAudio_LockOutput();

	closed = NativeAudio_FreeXA();
	NativeAudio_XAStreamSwap(&s_audio.xaStream, &s_audio.xaPendingStream);
	s_audio.xa.frameCount = frameCount;
	s_audio.xa.sampleRate = sampleRate;
	s_audio.xa.categoryID = 0;
	s_audio.xa.xaID = 0;
	s_audio.xa.hasTrackIdentity = 0;
	s_audio.xa.positionFp = 0;
	s_audio.xa.outputFrame = 0;
	s_audio.xa.stepFp = (u32)(((u64)sampleRate << NATIVE_AUDIO_FP_SHIFT) / NATIVE_AUDIO_SAMPLE_RATE);
	s_audio.xa.volumeLeft = (s16)volumeLeft;
	s_audio.xa.volumeRight = (s16)volumeRight;
	s_audio.commonAttr.cd.volume.left = (s16)volumeLeft;
	s_audio.commonAttr.cd.volume.right = (s16)volumeRight;
	s_audio.commonAttr.mask |= SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR;
	s_audio.xa.active = 1;

	NativeAudio_UnlockOutput();

	if (closed.open)
	{
		NativeAudio_XASourceClose(&closed.source);
	}

	return 1;
}

int NativeAudio_GetXACurrOffset(void)
{
	u64 outputFrame;
	u64 outputFrameCount;
	int offset;

	NativeAudio_LockOutput();

	if ((s_audio.xa.hasTrackIdentity == 0) || (s_audio.xa.sampleRate <= 0))
	{
		offset = 0;
	}
	else
	{
		// NOTE(aalhendi): Retail XA_CurrOffset advances in decoded SPU blocks,
		// which cutscene lipsync converts with (offset * 30 * 0x100) / 44100.
		outputFrame = s_audio.xa.outputFrame;
		outputFrameCount = NativeAudio_GetXAOutputFrameCount(s_audio.xa.frameCount, s_audio.xa.sampleRate);
		if ((outputFrameCount > 0) && (outputFrame > outputFrameCount))
		{
			outputFrame = outputFrameCount;
		}
		outputFrame >>= 8;
		offset = outputFrame > (u64)INT_MAX ? INT_MAX : (int)outputFrame;
	}

	NativeAudio_UnlockOutput();

	return offset;
}

internal int NativeAudio_GetXAMaxSampleAtSourceFrameNoLock(u64 frameIndex)
{
	int max = 0;
	int frame;

	if (s_audio.xa.active)
	{
		for (frame = 0; frame < 0x80; frame++)
		{
			int left;
			int right;

			if (frameIndex + (u64)frame >= (u64)s_audio.xa.frameCount)
			{
				break;
			}

			left = NativeAudio_GetXAPcmSampleAtFrameNoLock(0, frameIndex + (u64)frame);
			right = NativeAudio_GetXAPcmSampleAtFrameNoLock(1, frameIndex + (u64)frame);

			if (left < 0)
			{
				left = -left;
			}
			if (right < 0)
			{
				right = -right;
			}

			if (max < left)
			{
				max = left;
			}
			if (max < right)
			{
				max = right;
			}
		}
	}

	return max;
}

int NativeAudio_GetXAMaxSample(void)
{
	int max;

	NativeAudio_LockOutput();

	max = NativeAudio_GetXAMaxSampleAtSourceFrameNoLock(s_audio.xa.positionFp >> NATIVE_AUDIO_FP_SHIFT);

	NativeAudio_UnlockOutput();

	return max;
}

int NativeAudio_GetXAMaxSampleAtOffset(int xaCurrOffset)
{
	u64 outputFrame;
	u64 sourceFrame;
	int max = 0;

	if (xaCurrOffset < 0)
	{
		return 0;
	}

	NativeAudio_LockOutput();

	if ((s_audio.xa.active != 0) && (s_audio.xa.sampleRate > 0))
	{
		outputFrame = (u64)xaCurrOffset << 8;
		sourceFrame = (outputFrame * (u64)s_audio.xa.sampleRate) / NATIVE_AUDIO_SAMPLE_RATE;
		max = NativeAudio_GetXAMaxSampleAtSourceFrameNoLock(sourceFrame);
	}

	NativeAudio_UnlockOutput();

	return max;
}

#if CTR_NATIVE_AUDIO_DECODE_PROFILE
/*
 * Voice-start profile plumbing for native_perf.c (same unity TU; perf
 * forward-declares both). Consume returns-and-resets the per-frame key-on
 * count -- called once per frame by the perf frame close for the
 * worst-frame/key-on correlation. Format emits the whole [PERF-VOICE]
 * payload and resets the window counters, so the report and the reset can
 * never drift apart. Integers only: devkitARM newlib does not link float
 * formatting unless asked, and a silently-blank %f costs a build-and-run
 * cycle to notice.
 */
internal int NativeAudio_DebugConsumeFrameKeyOns(void)
{
	int n = s_voiceProf.frameKeyOns;

	s_voiceProf.frameKeyOns = 0;
	return n;
}

internal void NativeAudio_DebugFormatVoiceProfWindow(char *buf, int bufSize, int maxFrameUs, int maxFrameKeyOns)
{
	snprintf(buf, (size_t)bufSize,
	         "[PERF-VOICE] keyOns=%d decodeCalls=%d decodedSamples=%d scannedBlocks=%d arenaGrows=%d arenaPeakKiB=%d maxFrameUs=%d maxFrameKeyOns=%d"
	         " hwResets=%d hwStaled=%d hwRefresh=%d spuWriteKiB=%d",
	         s_voiceProf.windowKeyOns, s_voiceProf.windowDecodeCalls, s_voiceProf.windowDecodedSamples, s_voiceProf.windowScannedBlocks,
	         s_voiceProf.windowArenaGrows, s_voiceProf.arenaPeakKiB, maxFrameUs, maxFrameKeyOns, s_voiceProf.windowHwResets, s_voiceProf.windowHwStaled,
	         s_voiceProf.windowHwRefresh, s_voiceProf.windowSpuWriteBytes >> 10);
	s_voiceProf.windowKeyOns = 0;
	s_voiceProf.windowDecodeCalls = 0;
	s_voiceProf.windowDecodedSamples = 0;
	s_voiceProf.windowScannedBlocks = 0;
	s_voiceProf.windowArenaGrows = 0;
	s_voiceProf.windowHwResets = 0;
	s_voiceProf.windowHwStaled = 0;
	s_voiceProf.windowHwRefresh = 0;
	s_voiceProf.windowSpuWriteBytes = 0;
}
#endif
