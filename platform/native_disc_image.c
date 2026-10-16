#include "platform/native_disc_image.h"

#include <platform/native_path.h>
#include <platform/native_perf.h>

#include <errno.h>
#include <limits.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif
#if defined(__3DS__)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_DISC_IMAGE_PATH_MAX          1024
#define NATIVE_DISC_IMAGE_BIN_PATH          "ctr-u.bin"
#define NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET 24u
#define NATIVE_DISC_IMAGE_FORM1_DATA_SIZE   2048u
#define NATIVE_DISC_IMAGE_MODE2_USER_OFFSET 16u
#define NATIVE_DISC_IMAGE_MODE2_USER_SIZE   2336u
#define NATIVE_DISC_IMAGE_PVD_LBA           16u
#define NATIVE_DISC_IMAGE_PVD_VOLUME_SIZE   80u
#define NATIVE_DISC_IMAGE_PVD_ROOT_RECORD   156u
#define NATIVE_DISC_IMAGE_DIRECTORY_FLAG    0x02u
/* The 2-second postgap a mastered data track ends with. It is inside the PVD's
 * volume size but holds no file data, and some rippers leave it off, so an
 * image may end this far short of the volume and still be complete. */
#define NATIVE_DISC_IMAGE_POSTGAP_SECTORS   150u

// NOTE(aalhendi): This hardcodes only the common NTSC-U raw BIN layout:
// one MODE2/2352 data track at byte zero. The user still supplies all disc
// contents; native only uses this sector contract to read the image.

struct NativeDiscImageDirRecord
{
	u32 lba;
	u32 size;
	u8 flags;
	u8 nameLen;
	const u8 *name;
};

global_variable char s_nativeDiscImagePath[NATIVE_DISC_IMAGE_PATH_MAX];
#if defined(__3DS__)
/*
 * On 3DS the image is read through raw devoptab fds, one per reader role
 * (NATIVE_DISC_IMAGE_OWNER_*), not through stdio. Two reasons:
 *
 * 1. The devoptab's seek(SEEK_SET) is pure local bookkeeping with no FS
 *    service call, and its read hands the stored offset straight to
 *    FSFILE_Read, so an lseek+read pair costs one SD round trip where
 *    fseek+fread cost the equivalent of two.
 *
 * 2. One fd per role deletes the shared-file-position hazard by construction,
 *    since each role's fd has exactly one thread using it at a time.
 */
global_variable int s_nativeDiscImageFd[NATIVE_DISC_IMAGE_OWNER_COUNT] = {-1, -1};
#else
global_variable FILE *s_nativeDiscImageFile;
#endif
global_variable struct NativeDiscImageFile s_nativeDiscImageRoot;
global_variable int s_nativeDiscImageAvailable;
global_variable struct NativeDiscImageInfo s_nativeDiscImageInfo;
global_variable int s_nativeDiscImageReadError[NATIVE_DISC_IMAGE_OWNER_COUNT];

/*
 * Scratch for every main-thread read path (level loads via NativeCD, STR,
 * whole-file reads). The audio worker owns its own -- see the header. If a
 * new disc reader ever appears on a third thread it needs its own scratch
 * too; there is no lock here and deliberately so, the fix for SD contention
 * is fewer reads, not serialized ones.
 */
global_variable struct NativeDiscImageScratch s_nativeDiscImageMainScratch = {NATIVE_DISC_IMAGE_OWNER_MAIN, {0}};

/* Path lookup cache -- see NativeDiscImage_FindFile for what it is for and
 * why it is main-thread only. Declared up here so Init can clear it. */
#define NATIVE_DISC_IMAGE_PATH_CACHE_ENTRIES 128
#define NATIVE_DISC_IMAGE_PATH_CACHE_KEY_MAX 96

struct NativeDiscImagePathCacheEntry
{
	char path[NATIVE_DISC_IMAGE_PATH_CACHE_KEY_MAX];
	struct NativeDiscImageFile file;
	u8 valid;
	u8 found;
};

global_variable struct NativeDiscImagePathCacheEntry s_nativeDiscImagePathCache[NATIVE_DISC_IMAGE_PATH_CACHE_ENTRIES];
global_variable u32 s_nativeDiscImagePathCacheNext;

#if CTR_NATIVE_DISC_IO_PROFILE
/*
 * Per-owner disc-read counters. Each bucket has exactly ONE writer thread
 * (that is what `owner` means), so the increments need no atomics; only the
 * reporter reads across threads, and a torn window count is acceptable in a
 * diagnostic that exists to size an effect, not to gate correctness.
 *
 * `headerFails` used to be the interesting one beyond the timings: with one
 * shared FILE*, fseek+fread was not atomic across the pair, so an
 * interleaved read could land on the other thread's position and fail the
 * sector-header check. On 3DS that hazard is gone by construction (one fd
 * per role, see s_nativeDiscImageFd), so a non-zero count here now means a
 * genuinely malformed read -- wrong image layout or a real I/O problem --
 * not contention.
 */
struct NativeDiscImageIoProf
{
	u32 calls;
	u32 sectors;
	u32 runsOf1;
	u32 maxRun;
	u32 headerFails;
	u32 ioFails;
	u64 ticks;
	u64 worstTicks;
	/*
	 * The seek/read split is the verdict instrument for the stdio-artifact
	 * theory: seekUs collapsing to ~0 confirms the seek itself was free and
	 * the flat per-call cost was a newlib stdio artifact; the cost migrating
	 * into readUs instead would point back at the FS-service round trip.
	 */
	u64 seekTicks;
	u64 readTicks;
	u64 worstSeekTicks;
	u64 worstReadTicks;
};

global_variable struct NativeDiscImageIoProf s_nativeDiscIoProf[NATIVE_DISC_IMAGE_OWNER_COUNT];

/*
 * Directory-walk reads, counted apart from payload. A mean sectors-per-call
 * of 1.7 can be one big batched load hiding behind two thousand one-sector
 * metadata reads, or it can be a load path that genuinely asks for 1.7
 * sectors at a time -- and those two want completely different fixes, so the
 * mean alone is not a finding. FindFile re-walks the tree from the root on
 * every open with nothing cached, which makes this the suspect worth
 * separating.
 */
global_variable u32 s_nativeDiscIoDirCalls;
global_variable u32 s_nativeDiscIoDirSectors;

/*
 * Call-site tallies. A high single-sector-read percentage says the batching
 * has nothing to batch, but not why, and the fix differs by caller, so these
 * name the caller instead of leaving it to be inferred. Coverage must stay
 * exhaustive, or an untagged site turns the whole instrument into a guess.
 * The audio owner bucket needs no tags (only caller is the XA worker; runs1
 * separates fallback singles from span fills). Declared here because
 * native_disc_image.c is first in the unity include order, so native_audio.c,
 * native_cd.c, and native_str.c can all reach them. Single writer each.
 */
global_variable u32 s_nativeDiscIoSiteCd;          /* NativeCD_ReadSectors -- the game's CdRead */
global_variable u32 s_nativeDiscIoSiteCdSectors;   /* ...and the sector counts it asks for */
global_variable u32 s_nativeDiscIoSiteXaCount;     /* NativeAudio_XAStreamCountAudioSectors' header scan */
global_variable u32 s_nativeDiscIoSiteSingle;      /* the single-sector helper (PVD / root lookup) */
global_variable u32 s_nativeDiscIoSiteFile;        /* NativeDiscImage_ReadFileBytes -- whole-file asset reads */
global_variable u32 s_nativeDiscIoSiteFileSectors; /* ...and the sector counts those files span */
global_variable u32 s_nativeDiscIoSiteStr;         /* native_str.c FMV read-ahead refills -- counts disc CALLS (each up to one chunk), NOT consumed sectors */
global_variable u32 s_nativeDiscIoSiteXaOne;       /* NativeAudio_XASourceReadSector main-bucket singles (open/prebuffer/PC pump) */

internal unsigned long long NativeDiscImage_ProfTick(void)
{
#if defined(__3DS__)
	return N3DS_GetTickCount();
#else
	return 0ull;
#endif
}

internal void NativeDiscImage_ProfAccumulate(struct NativeDiscImageIoProf *prof, unsigned long long beginTick)
{
	unsigned long long elapsed = NativeDiscImage_ProfTick() - beginTick;

	prof->ticks += elapsed;
	if (elapsed > prof->worstTicks)
	{
		prof->worstTicks = elapsed;
	}
}
#endif

internal int NativeDiscImage_FindHostImagePath(char *dst, size_t dstSize, NativeStr8 assetsDir)
{
#if defined(_WIN32)
	return NativePath_Join(dst, dstSize, assetsDir, NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH));
#else
	char dirPath[NATIVE_DISC_IMAGE_PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	int found = 0;

	if (!NativePath_NormalizeSlashes(dirPath, sizeof(dirPath), assetsDir))
	{
		return 0;
	}

	dir = opendir(dirPath);
	if (dir == NULL)
	{
		return 0;
	}

	while ((entry = readdir(dir)) != NULL)
	{
		NativeStr8 entryName = NativeStr8_FromCString(entry->d_name);

		if (!NativeStr8_EqualsIgnoreCaseAscii(entryName, NATIVE_STR8_LIT(NATIVE_DISC_IMAGE_BIN_PATH)))
		{
			continue;
		}

		found = NativePath_Join(dst, dstSize, NativeStr8_FromCString(dirPath), entryName);
		break;
	}

	closedir(dir);
	return found;
#endif
}

internal void NativeDiscImage_ImageClose(void)
{
#if defined(__3DS__)
	int i;

	for (i = 0; i < NATIVE_DISC_IMAGE_OWNER_COUNT; i++)
	{
		if (s_nativeDiscImageFd[i] >= 0)
		{
			close(s_nativeDiscImageFd[i]);
			s_nativeDiscImageFd[i] = -1;
		}
	}
#else
	if (s_nativeDiscImageFile != NULL)
	{
		fclose(s_nativeDiscImageFile);
		s_nativeDiscImageFile = NULL;
	}
#endif
}

internal int NativeDiscImage_ImageOpen(const char *path)
{
#if defined(__3DS__)
	/* Same read-only file opened once per role; if the first open succeeds
	 * the rest can only fail on handle exhaustion, which is worth failing
	 * loudly for rather than silently re-sharing one position. */
	int i;

	for (i = 0; i < NATIVE_DISC_IMAGE_OWNER_COUNT; i++)
	{
		s_nativeDiscImageFd[i] = open(path, O_RDONLY);
		if (s_nativeDiscImageFd[i] < 0)
		{
			NativeDiscImage_ImageClose();
			return 0;
		}
	}

	return 1;
#else
	s_nativeDiscImageFile = fopen(path, "rb");
	return s_nativeDiscImageFile != NULL;
#endif
}

internal int NativeDiscImage_ImageIsOpen(void)
{
#if defined(__3DS__)
	return s_nativeDiscImageFd[NATIVE_DISC_IMAGE_OWNER_MAIN] >= 0;
#else
	return s_nativeDiscImageFile != NULL;
#endif
}

#if defined(__3DS__)
/* One devoptab read maps to one FSFILE_Read of the full length, so this loop
 * runs once in practice; it exists so a short read is an error only when no
 * progress was made, not a silent truncation. */
internal int NativeDiscImage_FdReadFull(int fd, u8 *dst, size_t bytes)
{
	while (bytes != 0)
	{
		ssize_t got = read(fd, dst, bytes);

		if (got <= 0)
		{
			return 0;
		}

		dst += got;
		bytes -= (size_t)got;
	}

	return 1;
}
#endif

internal u32 NativeDiscImage_ReadLE32(const u8 *data)
{
	return ((u32)data[0]) | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

internal int NativeDiscImage_CheckRawSectorHeader(const u8 *sector)
{
	u32 i;

	if ((sector[0] != 0x00) || (sector[11] != 0x00) || (sector[15] != 0x02))
	{
		return 0;
	}

	for (i = 1; i < 11; i++)
	{
		if (sector[i] != 0xff)
		{
			return 0;
		}
	}

	return 1;
}

internal int NativeDiscImage_OwnerIndex(int owner)
{
	return (owner == NATIVE_DISC_IMAGE_OWNER_AUDIO) ? NATIVE_DISC_IMAGE_OWNER_AUDIO : NATIVE_DISC_IMAGE_OWNER_MAIN;
}

internal int NativeDiscImage_FailRead(int owner, int error)
{
	s_nativeDiscImageReadError[NativeDiscImage_OwnerIndex(owner)] = error;
	return 0;
}

/*
 * THE choke point: every byte the game reads off the disc image arrives
 * through here, and each call is one seek plus one read. Reading a whole run
 * at once is the entire point -- keep it that way; do not reintroduce a
 * per-sector loop above a per-sector read.
 */
internal int NativeDiscImage_ReadRawRun(u32 lba, u32 count, u8 *raw, int owner)
{
	u64 offset;
	size_t bytes;
	u32 i;
#if CTR_NATIVE_DISC_IO_PROFILE
	struct NativeDiscImageIoProf *prof = &s_nativeDiscIoProf[(owner == NATIVE_DISC_IMAGE_OWNER_AUDIO) ? NATIVE_DISC_IMAGE_OWNER_AUDIO : NATIVE_DISC_IMAGE_OWNER_MAIN];
	unsigned long long beginTick;
#else
	(void)owner;
#endif

	if (!NativeDiscImage_ImageIsOpen() || (count == 0))
	{
		return NativeDiscImage_FailRead(owner, NATIVE_DISC_IMAGE_READ_IO_FAILED);
	}

	offset = (u64)lba * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
	bytes = (size_t)count * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;

#if CTR_NATIVE_DISC_IO_PROFILE
	prof->calls++;
	prof->sectors += count;
	if (count == 1u)
	{
		prof->runsOf1++;
	}
	if (count > prof->maxRun)
	{
		prof->maxRun = count;
	}
	beginTick = NativeDiscImage_ProfTick();
#endif

	if (offset > (u64)LONG_MAX)
	{
#if CTR_NATIVE_DISC_IO_PROFILE
		prof->ioFails++;
#endif
		return NativeDiscImage_FailRead(owner, NATIVE_DISC_IMAGE_READ_PAST_IMAGE_END);
	}

	/*
	 * Seek and read are timed apart, and deliberately NOT folded back into
	 * one condition: the split is the measurement. Three tick reads total
	 * (begin / after seek / after read), so the combined us and worstUs
	 * fields stay exactly what the previous hardware run reported. On 3DS
	 * the seek is now a devoptab lseek -- local bookkeeping, no FS call --
	 * so seekUs collapsing to ~0 on the next hardware run confirms the
	 * stdio-artifact theory; the cost reappearing in readUs refutes it.
	 */
	{
#if defined(__3DS__)
		int fd = s_nativeDiscImageFd[(owner == NATIVE_DISC_IMAGE_OWNER_AUDIO) ? NATIVE_DISC_IMAGE_OWNER_AUDIO : NATIVE_DISC_IMAGE_OWNER_MAIN];
		int seekFailed = (lseek(fd, (off_t)offset, SEEK_SET) < 0);
#else
		int seekFailed = (fseek(s_nativeDiscImageFile, (long)offset, SEEK_SET) != 0);
#endif
#if CTR_NATIVE_DISC_IO_PROFILE
		unsigned long long midTick = NativeDiscImage_ProfTick();
		unsigned long long seekElapsed = midTick - beginTick;

		prof->seekTicks += seekElapsed;
		if (seekElapsed > prof->worstSeekTicks)
		{
			prof->worstSeekTicks = seekElapsed;
		}
#endif

		if (seekFailed)
		{
#if CTR_NATIVE_DISC_IO_PROFILE
			prof->ioFails++;
			NativeDiscImage_ProfAccumulate(prof, beginTick);
#endif
			return NativeDiscImage_FailRead(owner, NATIVE_DISC_IMAGE_READ_IO_FAILED);
		}

		int readOk;
#if defined(__3DS__)
		/* Time the MAIN-thread read only (owner=MAIN). The audio-thread XA
		 * streaming reads use owner=AUDIO on their own fd and must never touch
		 * this main-thread scope -- see the DISC_READ_MAIN enum comment. */
		if (owner == NATIVE_DISC_IMAGE_OWNER_MAIN)
		{
			NativePerf_BeginScope(NATIVE_PERF_BUCKET_DISC_READ_MAIN);
		}
		readOk = NativeDiscImage_FdReadFull(fd, raw, bytes);
		if (owner == NATIVE_DISC_IMAGE_OWNER_MAIN)
		{
			NativePerf_EndScope(NATIVE_PERF_BUCKET_DISC_READ_MAIN);
		}
#else
		readOk = (fread(raw, 1, bytes, s_nativeDiscImageFile) == bytes);
#endif
		if (!readOk)
		{
#if CTR_NATIVE_DISC_IO_PROFILE
			prof->ioFails++;
			prof->readTicks += NativeDiscImage_ProfTick() - midTick;
			NativeDiscImage_ProfAccumulate(prof, beginTick);
#endif
			if ((s_nativeDiscImageInfo.imageBytes != 0) && (offset + bytes > s_nativeDiscImageInfo.imageBytes))
			{
				return NativeDiscImage_FailRead(owner, NATIVE_DISC_IMAGE_READ_PAST_IMAGE_END);
			}
			return NativeDiscImage_FailRead(owner, NATIVE_DISC_IMAGE_READ_IO_FAILED);
		}

#if CTR_NATIVE_DISC_IO_PROFILE
		{
			unsigned long long endTick = NativeDiscImage_ProfTick();
			unsigned long long readElapsed = endTick - midTick;
			unsigned long long totalElapsed = endTick - beginTick;

			prof->readTicks += readElapsed;
			if (readElapsed > prof->worstReadTicks)
			{
				prof->worstReadTicks = readElapsed;
			}

			prof->ticks += totalElapsed;
			if (totalElapsed > prof->worstTicks)
			{
				prof->worstTicks = totalElapsed;
			}
		}
#endif
	}

	for (i = 0; i < count; i++)
	{
		if (!NativeDiscImage_CheckRawSectorHeader(&raw[(size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE]))
		{
#if CTR_NATIVE_DISC_IO_PROFILE
			prof->headerFails++;
#endif
			return NativeDiscImage_FailRead(owner, NATIVE_DISC_IMAGE_READ_BAD_SECTOR);
		}
	}

	s_nativeDiscImageReadError[NativeDiscImage_OwnerIndex(owner)] = NATIVE_DISC_IMAGE_READ_OK;
	return 1;
}

internal int NativeDiscImage_ReadRawSector(u32 lba, u8 *sector)
{
#if CTR_NATIVE_DISC_IO_PROFILE
	s_nativeDiscIoSiteSingle++;
#endif
	return NativeDiscImage_ReadRawRun(lba, 1, sector, NATIVE_DISC_IMAGE_OWNER_MAIN);
}

internal int NativeDiscImage_ParseDirRecord(const u8 *src, size_t available, struct NativeDiscImageDirRecord *record)
{
	u8 length;

	if ((src == NULL) || (record == NULL) || (available < 34))
	{
		return 0;
	}

	length = src[0];
	if ((length == 0) || (length > available) || (length < 34))
	{
		return 0;
	}

	record->lba = NativeDiscImage_ReadLE32(&src[2]);
	record->size = NativeDiscImage_ReadLE32(&src[10]);
	record->flags = src[25];
	record->nameLen = src[32];
	record->name = &src[33];

	if ((u32)record->nameLen + 33u > length)
	{
		return 0;
	}

	return 1;
}

internal int NativeDiscImage_NameEquals(const struct NativeDiscImageDirRecord *record, NativeStr8 component)
{
	size_t recordLen;
	size_t componentLen;
	size_t i;

	if ((record == NULL) || (record->name == NULL) || (component.ptr == NULL))
	{
		return 0;
	}

	recordLen = record->nameLen;
	componentLen = component.len;

	while ((recordLen > 0) && (record->name[recordLen - 1u] == ' '))
	{
		recordLen--;
	}

	if ((recordLen > 2) && (record->name[recordLen - 2u] == ';') && (record->name[recordLen - 1u] == '1'))
	{
		recordLen -= 2;
	}

	if ((componentLen > 2) && (component.ptr[componentLen - 2u] == ';') && (component.ptr[componentLen - 1u] == '1'))
	{
		componentLen -= 2;
	}

	if (recordLen != componentLen)
	{
		return 0;
	}

	for (i = 0; i < componentLen; i++)
	{
		if (NativeStr8_ToUpperAscii(record->name[i]) != NativeStr8_ToUpperAscii(component.ptr[i]))
		{
			return 0;
		}
	}

	return 1;
}

internal NativeStr8 NativeDiscImage_NextPathComponent(NativeStr8 *path)
{
	NativeStr8 result = *path;
	size_t i;

	while ((result.len != 0) && NativePath_IsSeparator(result.ptr[0]))
	{
		result = NativeStr8_Skip(result, 1);
	}

	for (i = 0; i < result.len; i++)
	{
		if (NativePath_IsSeparator(result.ptr[i]))
		{
			NativeStr8 component = {result.ptr, i};
			*path = NativeStr8_Skip(result, i + 1u);
			return component;
		}
	}

	*path = NativeStr8_Skip(result, result.len);
	return result;
}

internal u32 NativeDiscImage_DataSectorCount(u32 size)
{
	return (size + (NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1u)) / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
}

internal u32 NativeDiscImage_RawSectorCount(u32 size)
{
	if ((size != 0) && ((size % NATIVE_DISC_IMAGE_MODE2_USER_SIZE) == 0))
	{
		return size / NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
	}

	return NativeDiscImage_DataSectorCount(size);
}

internal int NativeDiscImage_ReadDirectoryBytes(const struct NativeDiscImageFile *dir, u8 **dataOut, int *sizeOut)
{
	u32 sectorCount;
	u8 *data;

	*dataOut = NULL;
	*sizeOut = 0;

	if ((dir == NULL) || (dir->size == 0) || (dir->size > 0x7fffffff))
	{
		return 0;
	}

	sectorCount = NativeDiscImage_DataSectorCount(dir->size);
	if (sectorCount == 0)
	{
		return 0;
	}

	data = (u8 *)malloc((size_t)sectorCount * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE);
	if (data == NULL)
	{
		return 0;
	}

	/* Batched like every other multi-sector read. This one is easy to write
	 * off as small -- a directory is a handful of sectors -- but FindFile
	 * re-walks the tree from the root on EVERY file open, one
	 * ReadDirectoryBytes per path component, and nothing caches the result.
	 * Main-thread only, like FindFile itself. */
#if CTR_NATIVE_DISC_IO_PROFILE
	s_nativeDiscIoDirCalls++;
	s_nativeDiscIoDirSectors += sectorCount;
#endif

	if (!NativeDiscImage_ReadDataSectorsScratch(dir, 0, sectorCount, data, &s_nativeDiscImageMainScratch))
	{
		free(data);
		return 0;
	}

	*dataOut = data;
	*sizeOut = (int)dir->size;
	return 1;
}

internal int NativeDiscImage_FindInDirectory(const struct NativeDiscImageFile *dir, NativeStr8 component, struct NativeDiscImageFile *fileOut, u8 *flagsOut)
{
	u8 *data;
	int size;
	int offset;
	int found = 0;

	if (!NativeDiscImage_ReadDirectoryBytes(dir, &data, &size))
	{
		return 0;
	}

	offset = 0;
	while (offset < size)
	{
		struct NativeDiscImageDirRecord record;
		u8 length = data[offset];

		if (length == 0)
		{
			offset = (offset + (int)NATIVE_DISC_IMAGE_FORM1_DATA_SIZE) & ~((int)NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1);
			continue;
		}

		if ((length < 34) || (offset + length > size))
		{
			break;
		}

		if (NativeDiscImage_ParseDirRecord(&data[offset], (size_t)(size - offset), &record) && NativeDiscImage_NameEquals(&record, component))
		{
			fileOut->lba = record.lba;
			fileOut->size = record.size;
			*flagsOut = record.flags;
			found = 1;
			break;
		}

		offset += length;
	}

	free(data);
	return found;
}

internal int NativeDiscImage_LoadRoot(void)
{
	u8 sector[NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE];
	struct NativeDiscImageDirRecord root;

	if (!NativeDiscImage_ReadRawSector(NATIVE_DISC_IMAGE_PVD_LBA, sector))
	{
		return 0;
	}

	if ((memcmp(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + 1], "CD001", 5) != 0) || (sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET] != 1) ||
	    (sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + 6] != 1))
	{
		return 0;
	}

	if (!NativeDiscImage_ParseDirRecord(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + NATIVE_DISC_IMAGE_PVD_ROOT_RECORD],
	                                    NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - NATIVE_DISC_IMAGE_PVD_ROOT_RECORD, &root))
	{
		return 0;
	}

	s_nativeDiscImageRoot.lba = root.lba;
	s_nativeDiscImageRoot.size = root.size;
	s_nativeDiscImageInfo.volumeSectors = NativeDiscImage_ReadLE32(&sector[NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + NATIVE_DISC_IMAGE_PVD_VOLUME_SIZE]);
	return 1;
}

/* Size of the open image file in bytes, or 0 when it cannot be measured. */
internal u64 NativeDiscImage_ImageSizeBytes(void)
{
#if defined(__3DS__)
	off_t end = lseek(s_nativeDiscImageFd[NATIVE_DISC_IMAGE_OWNER_MAIN], 0, SEEK_END);

	return (end > 0) ? (u64)end : 0;
#else
	long end;

	if (fseek(s_nativeDiscImageFile, 0, SEEK_END) != 0)
	{
		return 0;
	}

	end = ftell(s_nativeDiscImageFile);
	return (end > 0) ? (u64)end : 0;
#endif
}

internal int NativeDiscImage_ReadImageBytes(u64 offset, u8 *dst, size_t bytes)
{
	if (offset > (u64)LONG_MAX)
	{
		return 0;
	}

#if defined(__3DS__)
	{
		int fd = s_nativeDiscImageFd[NATIVE_DISC_IMAGE_OWNER_MAIN];

		return (lseek(fd, (off_t)offset, SEEK_SET) >= 0) && NativeDiscImage_FdReadFull(fd, dst, bytes);
	}
#else
	return (fseek(s_nativeDiscImageFile, (long)offset, SEEK_SET) == 0) && (fread(dst, 1, bytes, s_nativeDiscImageFile) == bytes);
#endif
}

/* A cooked 2048-byte-sector image keeps its PVD at byte 16 * 2048. */
internal int NativeDiscImage_LooksLikeCookedIso(void)
{
	u8 header[7];

	if (!NativeDiscImage_ReadImageBytes((u64)NATIVE_DISC_IMAGE_PVD_LBA * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE, header, sizeof(header)))
	{
		return 0;
	}

	return (header[0] == 1) && (memcmp(&header[1], "CD001", 5) == 0) && (header[6] == 1);
}

internal int NativeDiscImage_Refuse(int status)
{
	s_nativeDiscImageInfo.status = status;
	NativeDiscImage_ImageClose();
	return 0;
}

int NativeDiscImage_Init(const char *assetsDir)
{
	char path[NATIVE_DISC_IMAGE_PATH_MAX];
	u64 imageSectors;

	s_nativeDiscImageAvailable = 0;
	s_nativeDiscImagePath[0] = '\0';
	memset(&s_nativeDiscImageInfo, 0, sizeof(s_nativeDiscImageInfo));
	s_nativeDiscImageInfo.status = NATIVE_DISC_IMAGE_STATUS_NOT_FOUND;

	/* A re-Init can point at a different image, so entries from the old one
	 * must not survive it. Cheap: this runs once in practice. */
	memset(s_nativeDiscImagePathCache, 0, sizeof(s_nativeDiscImagePathCache));
	s_nativeDiscImagePathCacheNext = 0;

	NativeDiscImage_ImageClose();

	if ((assetsDir == NULL) || !NativeDiscImage_FindHostImagePath(path, sizeof(path), NativeStr8_FromCString(assetsDir)))
	{
		return 0;
	}

	if (!NativeDiscImage_ImageOpen(path))
	{
		/* Windows names the image without listing the directory, so a missing
		 * file only shows up here. */
		if (errno != ENOENT)
		{
			NativePath_NormalizeSlashes(s_nativeDiscImageInfo.path, sizeof(s_nativeDiscImageInfo.path), NativeStr8_FromCString(path));
			s_nativeDiscImageInfo.status = NATIVE_DISC_IMAGE_STATUS_OPEN_FAILED;
		}
		return 0;
	}

	NativePath_NormalizeSlashes(s_nativeDiscImageInfo.path, sizeof(s_nativeDiscImageInfo.path), NativeStr8_FromCString(path));
	s_nativeDiscImageInfo.imageBytes = NativeDiscImage_ImageSizeBytes();

	if (!NativeDiscImage_LoadRoot())
	{
		return NativeDiscImage_Refuse(NativeDiscImage_LooksLikeCookedIso() ? NATIVE_DISC_IMAGE_STATUS_COOKED_ISO : NATIVE_DISC_IMAGE_STATUS_NO_VOLUME);
	}

	/* A short copy otherwise boots and only fails when a load first reaches
	 * past the cut, so it is refused before anything starts. */
	imageSectors = s_nativeDiscImageInfo.imageBytes / NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
	if ((s_nativeDiscImageInfo.imageBytes != 0) && (imageSectors + NATIVE_DISC_IMAGE_POSTGAP_SECTORS < s_nativeDiscImageInfo.volumeSectors))
	{
		return NativeDiscImage_Refuse(NATIVE_DISC_IMAGE_STATUS_TRUNCATED);
	}

	if (!NativePath_NormalizeSlashes(s_nativeDiscImagePath, sizeof(s_nativeDiscImagePath), NativeStr8_FromCString(path)))
	{
		return NativeDiscImage_Refuse(NATIVE_DISC_IMAGE_STATUS_OPEN_FAILED);
	}

	s_nativeDiscImageInfo.status = NATIVE_DISC_IMAGE_STATUS_OK;
	s_nativeDiscImageAvailable = 1;
	return 1;
}

const struct NativeDiscImageInfo *NativeDiscImage_GetInfo(void)
{
	return &s_nativeDiscImageInfo;
}

int NativeDiscImage_FileFitsImage(const struct NativeDiscImageFile *file)
{
	u64 endSector;

	if ((file == NULL) || (s_nativeDiscImageInfo.imageBytes == 0))
	{
		return 1;
	}

	endSector = (u64)file->lba + NativeDiscImage_DataSectorCount(file->size);
	return endSector * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE <= s_nativeDiscImageInfo.imageBytes;
}

int NativeDiscImage_LastReadError(int owner)
{
	return s_nativeDiscImageReadError[NativeDiscImage_OwnerIndex(owner)];
}

/*
 * Path lookup cache. Every FindFile re-walks the ISO tree from the root, one
 * ReadDirectoryBytes per path component, with nothing remembering the answer.
 * Negative results are cached too, since NativeAssets probes for files not on
 * the disc and a miss costs a full tree walk.
 *
 * Read-only image, so no invalidation beyond Init clearing it; round-robin
 * eviction so a boot-time working set can't pin the table. MAIN THREAD ONLY,
 * like every FindFile caller.
 */
internal struct NativeDiscImagePathCacheEntry *NativeDiscImage_PathCacheFind(NativeStr8 key)
{
	u32 i;

	for (i = 0; i < NATIVE_DISC_IMAGE_PATH_CACHE_ENTRIES; i++)
	{
		struct NativeDiscImagePathCacheEntry *entry = &s_nativeDiscImagePathCache[i];

		/* Case-insensitive, matching NativeDiscImage_NameEquals -- otherwise
		 * two spellings of one path would occupy two entries and, worse, a
		 * hit could disagree with what a walk would have returned. */
		if (entry->valid && NativeStr8_EqualsIgnoreCaseAscii(NativeStr8_FromCString(entry->path), key))
		{
			return entry;
		}
	}

	return NULL;
}

internal void NativeDiscImage_PathCacheStore(NativeStr8 key, int found, const struct NativeDiscImageFile *file)
{
	struct NativeDiscImagePathCacheEntry *entry;

	if (key.len >= NATIVE_DISC_IMAGE_PATH_CACHE_KEY_MAX)
	{
		return; /* too long to key on; it just keeps walking, which is correct */
	}

	entry = &s_nativeDiscImagePathCache[s_nativeDiscImagePathCacheNext % NATIVE_DISC_IMAGE_PATH_CACHE_ENTRIES];
	s_nativeDiscImagePathCacheNext++;

	memcpy(entry->path, key.ptr, key.len);
	entry->path[key.len] = '\0';
	entry->valid = 1;
	entry->found = (u8)(found != 0);
	if (found)
	{
		entry->file = *file;
	}
}

int NativeDiscImage_FindFile(const char *path, struct NativeDiscImageFile *fileOut)
{
	NativeStr8 remaining = NativePath_SkipLeadingSeparators(NativeStr8_FromCString(path));
	struct NativeDiscImageFile current = s_nativeDiscImageRoot;
	u8 flags = NATIVE_DISC_IMAGE_DIRECTORY_FLAG;
	NativeStr8 cacheKey;
	struct NativeDiscImagePathCacheEntry *cached;

	if (!s_nativeDiscImageAvailable || (path == NULL) || (fileOut == NULL))
	{
		return 0;
	}

	/* Key on the separator-stripped form, so "/FOO" and "FOO" share one entry. */
	cacheKey = remaining;
	cached = NativeDiscImage_PathCacheFind(cacheKey);
	if (cached != NULL)
	{
		if (cached->found)
		{
			*fileOut = cached->file;
		}
		return cached->found;
	}

	while (remaining.len != 0)
	{
		NativeStr8 component = NativeDiscImage_NextPathComponent(&remaining);

		if (component.len == 0)
		{
			continue;
		}

		if ((flags & NATIVE_DISC_IMAGE_DIRECTORY_FLAG) == 0)
		{
			NativeDiscImage_PathCacheStore(cacheKey, 0, NULL);
			return 0;
		}

		if (!NativeDiscImage_FindInDirectory(&current, component, &current, &flags))
		{
			NativeDiscImage_PathCacheStore(cacheKey, 0, NULL);
			return 0;
		}
	}

	if ((flags & NATIVE_DISC_IMAGE_DIRECTORY_FLAG) != 0)
	{
		NativeDiscImage_PathCacheStore(cacheKey, 0, NULL);
		return 0;
	}

	NativeDiscImage_PathCacheStore(cacheKey, 1, &current);
	*fileOut = current;
	return 1;
}

internal int NativeDiscImage_ReadDataBytesScratch(const struct NativeDiscImageFile *file, u32 offset, void *dst, size_t size, struct NativeDiscImageScratch *scratch)
{
	u8 *out = (u8 *)dst;

	if ((file == NULL) || (dst == NULL) || (scratch == NULL) || ((u64)offset + size > file->size))
	{
		return 0;
	}

	while (size != 0)
	{
		u32 sectorIndex = offset / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u32 sectorOffset = offset % NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u64 touched = ((u64)sectorOffset + size + NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - 1u) / NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		u32 run = (touched > NATIVE_DISC_IMAGE_CHUNK_SECTORS) ? NATIVE_DISC_IMAGE_CHUNK_SECTORS : (u32)touched;
		u32 i;

		if (!NativeDiscImage_ReadRawRun(file->lba + sectorIndex, run, scratch->raw, scratch->owner))
		{
			return 0;
		}

		for (i = 0; (i < run) && (size != 0); i++)
		{
			size_t copySize = NATIVE_DISC_IMAGE_FORM1_DATA_SIZE - sectorOffset;

			if (copySize > size)
			{
				copySize = size;
			}

			memcpy(out, &scratch->raw[((size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) + NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET + sectorOffset], copySize);
			out += copySize;
			offset += (u32)copySize;
			size -= copySize;
			sectorOffset = 0;
		}
	}

	return 1;
}

int NativeDiscImage_ReadDataSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch)
{
	u8 *out = (u8 *)dst;
	u32 fileSectorCount;

	if ((file == NULL) || (dst == NULL) || (scratch == NULL))
	{
		return 0;
	}

	fileSectorCount = NativeDiscImage_DataSectorCount(file->size);
	if ((sector > fileSectorCount) || (sectorCount > fileSectorCount - sector))
	{
		return NativeDiscImage_FailRead(scratch->owner, NATIVE_DISC_IMAGE_READ_OUTSIDE_FILE);
	}

	while (sectorCount != 0)
	{
		u32 run = (sectorCount > NATIVE_DISC_IMAGE_CHUNK_SECTORS) ? NATIVE_DISC_IMAGE_CHUNK_SECTORS : sectorCount;
		u32 i;

		if (!NativeDiscImage_ReadRawRun(file->lba + sector, run, scratch->raw, scratch->owner))
		{
			return 0;
		}

		for (i = 0; i < run; i++)
		{
			memcpy(&out[(size_t)i * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE], &scratch->raw[((size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) + NATIVE_DISC_IMAGE_FORM1_DATA_OFFSET],
			       NATIVE_DISC_IMAGE_FORM1_DATA_SIZE);
		}

		out += (size_t)run * NATIVE_DISC_IMAGE_FORM1_DATA_SIZE;
		sector += run;
		sectorCount -= run;
	}

	return 1;
}

int NativeDiscImage_ReadRawSectorsScratch(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst, struct NativeDiscImageScratch *scratch)
{
	u8 *out = (u8 *)dst;
	u32 fileSectorCount;

	if ((file == NULL) || (dst == NULL) || (scratch == NULL))
	{
		return 0;
	}

	fileSectorCount = NativeDiscImage_RawSectorCount(file->size);
	if ((sector > fileSectorCount) || (sectorCount > fileSectorCount - sector))
	{
		return NativeDiscImage_FailRead(scratch->owner, NATIVE_DISC_IMAGE_READ_OUTSIDE_FILE);
	}

	while (sectorCount != 0)
	{
		u32 run = (sectorCount > NATIVE_DISC_IMAGE_CHUNK_SECTORS) ? NATIVE_DISC_IMAGE_CHUNK_SECTORS : sectorCount;
		u32 i;

		if (!NativeDiscImage_ReadRawRun(file->lba + sector, run, scratch->raw, scratch->owner))
		{
			return 0;
		}

		for (i = 0; i < run; i++)
		{
			memcpy(&out[(size_t)i * NATIVE_DISC_IMAGE_MODE2_USER_SIZE], &scratch->raw[((size_t)i * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE) + NATIVE_DISC_IMAGE_MODE2_USER_OFFSET],
			       NATIVE_DISC_IMAGE_MODE2_USER_SIZE);
		}

		out += (size_t)run * NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
		sector += run;
		sectorCount -= run;
	}

	return 1;
}

int NativeDiscImage_ReadDataSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst)
{
	return NativeDiscImage_ReadDataSectorsScratch(file, sector, sectorCount, dst, &s_nativeDiscImageMainScratch);
}

int NativeDiscImage_ReadRawSectors(const struct NativeDiscImageFile *file, u32 sector, u32 sectorCount, void *dst)
{
	return NativeDiscImage_ReadRawSectorsScratch(file, sector, sectorCount, dst, &s_nativeDiscImageMainScratch);
}

int NativeDiscImage_ReadFileBytes(const char *path, int rawSectors, u8 **dataOut, int *sizeOut)
{
	struct NativeDiscImageFile file;
	u32 sectorCount;
	u32 size;
	u8 *data;

	*dataOut = NULL;
	*sizeOut = 0;

	if (!NativeDiscImage_FindFile(path, &file))
	{
		return 0;
	}

	if (rawSectors)
	{
		sectorCount = NativeDiscImage_RawSectorCount(file.size);
		size = sectorCount * NATIVE_DISC_IMAGE_MODE2_USER_SIZE;
	}
	else
	{
		sectorCount = NativeDiscImage_DataSectorCount(file.size);
		size = file.size;
	}

	if ((sectorCount == 0) || (size > 0x7fffffffu))
	{
		return 0;
	}

#if CTR_NATIVE_DISC_IO_PROFILE
	s_nativeDiscIoSiteFile++;
	s_nativeDiscIoSiteFileSectors += sectorCount;
#endif

	data = (u8 *)malloc((size_t)size);
	if (data == NULL)
	{
		return 0;
	}

	if (rawSectors)
	{
		if (!NativeDiscImage_ReadRawSectors(&file, 0, sectorCount, data))
		{
			free(data);
			return 0;
		}
	}
	else if (!NativeDiscImage_ReadDataBytesScratch(&file, 0, data, size, &s_nativeDiscImageMainScratch))
	{
		free(data);
		return 0;
	}

	*dataOut = data;
	*sizeOut = (int)size;
	return 1;
}

#if CTR_NATIVE_DISC_IO_PROFILE
/*
 * One [DISCIO] line per owner for native_perf.c's window report (same unity
 * TU; perf forward-declares this). Window counters reset inside the format
 * call so the report and the reset cannot drift apart -- same contract as
 * NativeAudio_DebugFormatVoiceProfWindow. Integers only: devkitARM newlib
 * does not link float formatting unless asked.
 *
 * `perCall` is the number to read first: it is sectors/calls, so it says
 * directly whether a read run actually batched (~16 for a level load) or
 * collapsed back to one sector per SD round trip (1).
 */
internal void NativeDiscImage_DebugFormatIoProfWindow(char *buf, int bufSize, int owner)
{
	struct NativeDiscImageIoProf *prof = &s_nativeDiscIoProf[(owner == NATIVE_DISC_IMAGE_OWNER_AUDIO) ? NATIVE_DISC_IMAGE_OWNER_AUDIO : NATIVE_DISC_IMAGE_OWNER_MAIN];
	unsigned long long freq = 1ull;
	u32 perCall;

#if defined(__3DS__)
	freq = N3DS_GetTickFrequency();
#endif
	if (freq == 0ull)
	{
		freq = 1ull;
	}

	perCall = (prof->calls == 0) ? 0u : (prof->sectors / prof->calls);

	if (owner == NATIVE_DISC_IMAGE_OWNER_AUDIO)
	{
		snprintf(buf, (size_t)bufSize, "[DISCIO] audio calls=%u sec=%u perCall=%u runs1=%u maxRun=%u us=%d worstUs=%d seekUs=%d readUs=%d wSeekUs=%d wReadUs=%d hdrFail=%u ioFail=%u",
		         prof->calls, prof->sectors, perCall, prof->runsOf1, prof->maxRun, (int)((prof->ticks * 1000000ull) / freq), (int)((prof->worstTicks * 1000000ull) / freq),
		         (int)((prof->seekTicks * 1000000ull) / freq), (int)((prof->readTicks * 1000000ull) / freq), (int)((prof->worstSeekTicks * 1000000ull) / freq),
		         (int)((prof->worstReadTicks * 1000000ull) / freq), prof->headerFails, prof->ioFails);
	}
	else
	{
		snprintf(buf, (size_t)bufSize,
		         "[DISCIO] main calls=%u sec=%u runs1=%u maxRun=%u dir=%u cd=%u/%u xaCnt=%u sng=%u file=%u/%u str=%u xa1=%u us=%d worstUs=%d seekUs=%d readUs=%d wSeekUs=%d wReadUs=%d "
		         "hdrFail=%u ioFail=%u",
		         prof->calls, prof->sectors, prof->runsOf1, prof->maxRun, s_nativeDiscIoDirCalls, s_nativeDiscIoSiteCd, s_nativeDiscIoSiteCdSectors, s_nativeDiscIoSiteXaCount,
		         s_nativeDiscIoSiteSingle, s_nativeDiscIoSiteFile, s_nativeDiscIoSiteFileSectors, s_nativeDiscIoSiteStr, s_nativeDiscIoSiteXaOne,
		         (int)((prof->ticks * 1000000ull) / freq), (int)((prof->worstTicks * 1000000ull) / freq), (int)((prof->seekTicks * 1000000ull) / freq),
		         (int)((prof->readTicks * 1000000ull) / freq), (int)((prof->worstSeekTicks * 1000000ull) / freq), (int)((prof->worstReadTicks * 1000000ull) / freq),
		         prof->headerFails, prof->ioFails);
		s_nativeDiscIoDirCalls = 0;
		s_nativeDiscIoDirSectors = 0;
		s_nativeDiscIoSiteCd = 0;
		s_nativeDiscIoSiteCdSectors = 0;
		s_nativeDiscIoSiteXaCount = 0;
		s_nativeDiscIoSiteSingle = 0;
		s_nativeDiscIoSiteFile = 0;
		s_nativeDiscIoSiteFileSectors = 0;
		s_nativeDiscIoSiteStr = 0;
		s_nativeDiscIoSiteXaOne = 0;
	}

	prof->calls = 0;
	prof->sectors = 0;
	prof->runsOf1 = 0;
	prof->maxRun = 0;
	prof->ticks = 0;
	prof->worstTicks = 0;
	prof->seekTicks = 0;
	prof->readTicks = 0;
	prof->worstSeekTicks = 0;
	prof->worstReadTicks = 0;
	prof->headerFails = 0;
	prof->ioFails = 0;
}
#endif
