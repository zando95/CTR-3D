#ifndef PLATFORM_NATIVE_AUDIO_H
#define PLATFORM_NATIVE_AUDIO_H

#include <macros.h>
#include <psx/libspu.h>

s32 NativeAudio_SpuInit(void);
u32 NativeAudio_SpuSetTransferStartAddr(u32 addr);
u32 NativeAudio_SpuWrite(const u8 *addr, u32 size);
void NativeAudio_SpuSetVoiceAttr(SpuVoiceAttr *psxAttrib);
void NativeAudio_SpuSetKey(s32 on_off, u32 voice_bit);
s32 NativeAudio_SpuSetReverb(s32 on_off);
s32 NativeAudio_SpuSetReverbModeParam(SpuReverbAttr *attr);
void NativeAudio_SpuSetReverbModeDepth(s16 left, s16 right);
u32 NativeAudio_SpuSetReverbVoice(s32 on_off, u32 voice_bit);
void NativeAudio_SpuSetCommonMasterVolume(s16 left, s16 right);
void NativeAudio_SpuSetCommonCDMix(s32 enabled);
void NativeAudio_SpuSetCommonCDVolume(s16 left, s16 right);
void NativeAudio_SpuSetCommonCDReverb(s32 enabled);

int NativeAudio_PlayXATrack(int categoryID, int xaID, int volumeLeft, int volumeRight);
// One-time (per boot) XA voice-line preload: reads every GAME-category track
// plus the wrong-way EXTRA barks into RAM so mid-race voice-line opens do zero
// disc reads. Resumable so the caller stays in control of the screen while it
// runs: Begin once, then Step (budgetSectors = max disc/SD sectors read per
// call, 0 = uncapped) until it returns nonzero, presenting between calls.
// Self-gates after the first Begin. No-op off the 3DS (PC disk reads never
// stalled a frame). Opens that happen before it completes take the cold path.
// Steps that only open a file or advance a stage read nothing; a caller that
// presents a frame per step pays a refresh for each of those. Compare
// SectorsRead across a Step to tell the two apart, and show WarmPercent (0..99
// against the read plan) while holding. Both stay 0 off the 3DS.
void NativeAudio_VoiceCacheEnsureBegin(void);
int NativeAudio_VoiceCacheEnsureStep(int budgetSectors);
u32 NativeAudio_VoiceCacheWarmSectorsRead(void);
int NativeAudio_VoiceCacheWarmPercent(void);
// After a language switch: refills the cache with the new language's voices when they differ (1 =
// started; step it with VoiceCacheEnsureStep like the boot warm, WarmPercent shows its progress).
// Always 0 off the 3DS.
int NativeAudio_VoiceCacheRebuildBegin(void);
int NativeAudio_PlayXAFile(const char *relativePath, int channelFilter, int volumeLeft, int volumeRight);
int NativeAudio_GetXATrackLength(int categoryID, int xaID);
void NativeAudio_SetVoiceLanguage(int languageFileIndex);
int NativeAudio_HasPalVoicePack(void);
void NativeAudio_PreloadVoiceManifest(int languageFileIndex);
int NativeAudio_IsXAPlaying(void);
int NativeAudio_GetXACurrOffset(void);
int NativeAudio_GetXAMaxSample(void);
int NativeAudio_GetXAMaxSampleAtOffset(int xaCurrOffset);
void NativeAudio_SetXAVolume(int volumeLeft, int volumeRight);
void NativeAudio_StopXA(void);
// Drops all decoded voice PCM and resets the decode arena. Needed on restart
// paths that reload no audio banks; see the comment on the definition.
void NativeAudio_ResetVoicePcmArena(void);

void NativeAudio_StepVBlank(void);
int NativeAudio_RenderFrames(s16 *out, int frameCount);
void NativeAudio_SetDeterministicRenderMode(int enabled);
int NativeAudio_IsDeterministicRenderMode(void);
int NativeAudio_QueueRenderedFrames(const s16 *frames, int frameCount);
void NativeAudio_ClearOutputQueue(void);
void NativeAudio_Shutdown(void);
#ifdef CTR_INTERNAL
void NativeAudio_GetOutputStats(int *underrunFrames, int *overflowFrames, int *queuedFrames);
#endif
#if defined(CTR_INTERNAL)
/*
 * Mixer golden-output regression harness -- see the block comment above the
 * implementation in platform/native_audio.c. Returns nonzero when it has run a
 * comparison and main() should exit without starting the game.
 */
int NativeAudio_GoldenConfigureFromArgs(int argc, char **argv);
void NativeAudio_GoldenCaptureTick(void);
#endif

int NativeAudio_GetStateSize(void);
int NativeAudio_CaptureState(void *dst, int dstSize);
int NativeAudio_RestoreState(const void *src, int srcSize);

#endif
