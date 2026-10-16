#ifndef NATIVE_DISC_IMAGE_H
#define NATIVE_DISC_IMAGE_H

#include <macros.h>

#include <stddef.h>

#define NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE 2352u

/*
 * Staging run for the bulk sector reader. Every disc-image read bottoms out
 * in one seek+read pair (stdio on PC, a raw per-role fd on 3DS); on 3DS the
 * read is an FS-service IPC round trip to the SD card, so what matters is
 * how many of them a read costs, not how many bytes it moves. Reading a run
 * of raw sectors into this buffer and splitting it into the caller's
 * (narrower) destination stride turns an N-sector read into
 * N/NATIVE_DISC_IMAGE_CHUNK_SECTORS reads.
 *
 * Caller-owned rather than a file-scope static because TWO threads read the
 * disc image on 3DS: the main thread (level loads, CD, STR) and the audio
 * worker (the XA pump, n3ds_audio_thread.c). Same disjointness contract as
 * the per-stream XA decode scratch -- whoever owns the scratch owns the read
 * -- which is the contract commit 87cfc24 had to introduce after a shared
 * static wedged run 5. The audio worker's stack is 32KB, so this cannot live
 * on the stack either.
 *
 * `owner` (NATIVE_DISC_IMAGE_OWNER_*) names a ROLE -- who the read is for --
 * not a thread: XA streaming vs everything else. On 3DS it is LOAD-BEARING,
 * not just a profile label: each role reads through its own devoptab fd
 * (each fd's position is only ever touched by the one thread running that
 * role at the time), which is what deletes the shared-file-position race.
 * Tagging a scratch with the wrong role puts its reads on the other role's
 * fd and quietly reintroduces that race. On PC both roles run on the vblank
 * thread through one FILE*, and the split only feeds the profile counters,
 * separating the music stream's steady drip from the level loader's bursts.
 */
/*
 * 64, raised from 16 on hardware evidence. An SD read
 * costs ~45ms per CALL and barely notices the size: measured, the read half
 * is ~26ms fixed + ~0.24ms per sector, and the seek half is ~11.5ms flat, so
 * 12.7x the data cost 14% more time. Both halves are per-call, so call count
 * is the only lever and this constant is the whole of it. 26,011 sectors went
 * 2,102 calls / 94.1s at 16; at 64 that is ~407 calls / ~21s.
 *
 * Costs 64 * 2352 = 147KB per scratch, and there are three (main, XA live, XA
 * pending) plus the XA header-scan chunk. Going higher keeps paying but less
 * each time (the fixed 26ms is amortised over more sectors), and each doubling
 * doubles that footprint AND the length of a single blocking read -- which the
 * audio worker feels directly, since a read it is inside is a read the DSP
 * queue is draining behind.
 */
#define NATIVE_DISC_IMAGE_CHUNK_SECTORS 64

#define NATIVE_DISC_IMAGE_OWNER_MAIN  0
#define NATIVE_DISC_IMAGE_OWNER_AUDIO 1
#define NATIVE_DISC_IMAGE_OWNER_COUNT 2

struct NativeDiscImageScratch
{
	int owner;
	u8 raw[NATIVE_DISC_IMAGE_CHUNK_SECTORS * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];
};

struct NativeDiscImageFile
{
	u32 lba;
	u32 size;
};

/* What NativeDiscImage_Init made of the image in the assets directory. Only
 * STATUS_OK makes the image readable; every other status names a reason it
 * was refused, for the startup error screen. */
enum NativeDiscImageStatus
{
	NATIVE_DISC_IMAGE_STATUS_NOT_FOUND,   /* no ctr-u.bin in the assets directory */
	NATIVE_DISC_IMAGE_STATUS_OPEN_FAILED, /* ctr-u.bin exists but could not be opened */
	NATIVE_DISC_IMAGE_STATUS_COOKED_ISO,  /* 2048-byte sectors: the XA and STR sectors are gone */
	NATIVE_DISC_IMAGE_STATUS_NO_VOLUME,   /* no ISO9660 volume in the raw MODE2/2352 layout */
	NATIVE_DISC_IMAGE_STATUS_TRUNCATED,   /* the file ends before the volume the PVD describes */
	NATIVE_DISC_IMAGE_STATUS_OK,
};

struct NativeDiscImageInfo
{
	int status;
	u64 imageBytes;    /* size of the image file; 0 if it could not be measured */
	u32 volumeSectors; /* PVD volume space size, once the PVD has been read */
	char path[1024];   /* the image file found, empty for STATUS_NOT_FOUND */
};

/* Why the last read through an owner's role failed. Each role has one writer
 * thread, so a role's own thread may read its entry without a lock. */
enum NativeDiscImageReadError
{
	NATIVE_DISC_IMAGE_READ_OK,
	NATIVE_DISC_IMAGE_READ_OUTSIDE_FILE,   /* request runs past the file's directory record */
	NATIVE_DISC_IMAGE_READ_PAST_IMAGE_END, /* the image file ends before the requested sector */
	NATIVE_DISC_IMAGE_READ_IO_FAILED,      /* the host seek or read failed */
	NATIVE_DISC_IMAGE_READ_BAD_SECTOR,     /* the raw sector's sync/mode header is wrong */
};

int NativeDiscImage_Init(const char *assetsDir);
const struct NativeDiscImageInfo *NativeDiscImage_GetInfo(void);
/* 1 when every sector of file lies inside the image file. */
int NativeDiscImage_FileFitsImage(const struct NativeDiscImageFile *file);
int NativeDiscImage_LastReadError(int owner);
int NativeDiscImage_FindFile(const char *path, struct NativeDiscImageFile *fileOut);

/*
 * The unsuffixed entry points read through a shared main-thread scratch and
 * are therefore MAIN-THREAD ONLY. Any caller that can run on the audio worker
 * must own a struct NativeDiscImageScratch and use the *Scratch variants.
 */
int NativeDiscImage_ReadDataSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst);
int NativeDiscImage_ReadRawSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst);
int NativeDiscImage_ReadDataSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch);
int NativeDiscImage_ReadRawSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch);
int NativeDiscImage_ReadFileBytes(const char *path, int rawSectors, u8 **dataOut, int *sizeOut);

#endif
