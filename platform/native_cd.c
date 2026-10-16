#include <macros.h>
#include <platform/native_assets.h>
#include <platform/native_cd.h>
#include <platform/native_disc_image.h>
#include <platform/native_fatal.h>
#include <platform/native_path.h>
#include <psx/libcd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// NOTE(aalhendi): Native exports the retail Cd* API. Extracted host files stay
// as dev/modding overrides; missing files can fall back to assets/ctr-u.bin.

#define NATIVE_CD_SECTOR_WORDS   512
#define NATIVE_CD_MAX_OPEN_FILES 8
#define NATIVE_CD_SECTOR_SIZE    0x800
#define NATIVE_CD_FILE_NAME_MAX  64

enum NativeCDFileSource
{
	NATIVE_CD_FILE_NONE,
	NATIVE_CD_FILE_HOST,
	NATIVE_CD_FILE_DISC,
};

struct NativeCDOpenFile
{
	int source;
	FILE *hostFile;
	struct NativeDiscImageFile discFile;
	char name[NATIVE_CD_FILE_NAME_MAX];
};

enum NativeCDHostReadError
{
	NATIVE_CD_HOST_READ_OK,
	NATIVE_CD_HOST_READ_PAST_END,
	NATIVE_CD_HOST_READ_IO_FAILED,
};

global_variable s32 s_cdDebugLevel;
global_variable s32 s_cdLastCom;
global_variable CdlCB s_cdReadyCallback;
global_variable CdlCB s_cdSyncCallback;
global_variable u32 s_cdSectorData[NATIVE_CD_SECTOR_WORDS];
global_variable CdlCB s_nativeCdReadCallback;
global_variable struct NativeCDOpenFile s_nativeCdFiles[NATIVE_CD_MAX_OPEN_FILES];
global_variable s32 s_nativeCdFileCount;
global_variable s32 s_nativeCdCurrentFile;
global_variable s32 s_nativeCdCurrentSector;
global_variable s32 s_nativeCdHostReadError;

int boolDecodeXaDuringVsyncCallback;

internal s32 NativeCD_NormalizeFilename(char *dst, s32 dstCount, const char *src)
{
	NativeStr8 filename = NativeStr8_FromCString(src);
	size_t i;

	if ((dstCount <= 0) || (src == NULL))
	{
		return 0;
	}

	for (i = 0; i < filename.len; i++)
	{
		if (filename.ptr[i] == ';')
		{
			filename.len = i;
			break;
		}
	}

	return NativePath_NormalizeSlashes(dst, (size_t)dstCount, filename);
}

internal NativeStr8 NativeCD_PathAfterRoot(NativeStr8 filename)
{
	if ((filename.len != 0) && NativePath_IsSeparator(filename.ptr[0]))
	{
		return NativeStr8_Skip(filename, 1);
	}

	return filename;
}

internal s32 NativeCD_OpenFile(const char *filename, s32 *outSize)
{
	char normalized[256];
	char rootlessPath[256];
	NativeStr8 rootless;
	FILE *file;
	struct NativeDiscImageFile discFile;
	s32 fileIndex;
	long fileSize;

	if (s_nativeCdFileCount >= NATIVE_CD_MAX_OPEN_FILES)
	{
		return -1;
	}

	if (NativeCD_NormalizeFilename(normalized, sizeof(normalized), filename) == 0)
	{
		return -1;
	}

	rootless = NativeCD_PathAfterRoot(NativeStr8_FromCString(normalized));

	file = NativeAssets_OpenHostStr8(rootless, "rb");
	if ((file == NULL) && NativeStr8_CopyToCString(rootlessPath, sizeof(rootlessPath), rootless))
	{
		file = fopen(rootlessPath, "rb");
	}

	if (file == NULL)
	{
		if (!NativeStr8_CopyToCString(rootlessPath, sizeof(rootlessPath), rootless) || !NativeDiscImage_FindFile(rootlessPath, &discFile))
		{
			return -1;
		}

		fileIndex = s_nativeCdFileCount++;
		memset(&s_nativeCdFiles[fileIndex], 0, sizeof(s_nativeCdFiles[fileIndex]));
		s_nativeCdFiles[fileIndex].source = NATIVE_CD_FILE_DISC;
		s_nativeCdFiles[fileIndex].discFile = discFile;
		NativeStr8_CopyToCString(s_nativeCdFiles[fileIndex].name, sizeof(s_nativeCdFiles[fileIndex].name), rootless);
		*outSize = (s32)discFile.size;
		return fileIndex;
	}

	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return -1;
	}

	fileSize = ftell(file);
	if (fileSize < 0)
	{
		fclose(file);
		return -1;
	}

	if (fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return -1;
	}

	fileIndex = s_nativeCdFileCount++;
	memset(&s_nativeCdFiles[fileIndex], 0, sizeof(s_nativeCdFiles[fileIndex]));
	s_nativeCdFiles[fileIndex].source = NATIVE_CD_FILE_HOST;
	s_nativeCdFiles[fileIndex].hostFile = file;
	NativeStr8_CopyToCString(s_nativeCdFiles[fileIndex].name, sizeof(s_nativeCdFiles[fileIndex].name), rootless);
	*outSize = (s32)fileSize;
	return fileIndex;
}

internal s32 NativeCD_SearchFile(CdlFILE *loc, const char *filename)
{
	char normalized[256];
	char rootlessPath[256];
	NativeStr8 rootless;
	s32 fileIndex;
	s32 fileSize;
	s32 encodedPos;

	if ((loc == NULL) || (filename == NULL))
	{
		return 0;
	}

	fileIndex = NativeCD_OpenFile(filename, &fileSize);
	if (fileIndex < 0)
	{
		return 0;
	}

	if (NativeCD_NormalizeFilename(normalized, sizeof(normalized), filename) == 0)
	{
		return 0;
	}

	rootless = NativeCD_PathAfterRoot(NativeStr8_FromCString(normalized));
	if (!NativeStr8_CopyToCString(rootlessPath, sizeof(rootlessPath), rootless))
	{
		return 0;
	}

	encodedPos = fileIndex << 24;

	memcpy(&loc->pos, &encodedPos, sizeof(encodedPos));
	loc->size = fileSize;
	memset(loc->name, 0, sizeof(loc->name));
	size_t nameLen = rootless.len;
	if (nameLen >= sizeof(loc->name))
	{
		nameLen = sizeof(loc->name) - 1;
	}
	memcpy(loc->name, rootless.ptr, nameLen);

	return 1;
}

internal s32 NativeCD_PosToInt(const CdlLOC *pos)
{
	s32 value;

	memcpy(&value, pos, sizeof(value));
	return value;
}

internal void NativeCD_IntToPos(s32 value, CdlLOC *pos)
{
	memcpy(pos, &value, sizeof(value));
}

internal s32 NativeCD_SetLoc(const CdlLOC *pos)
{
	s32 encodedPos;
	s32 fileIndex;
	s32 sector;

	encodedPos = NativeCD_PosToInt(pos);
	fileIndex = (encodedPos >> 24) & 0xff;
	sector = encodedPos & 0xffffff;

	if ((fileIndex < 0) || (fileIndex >= s_nativeCdFileCount) || (s_nativeCdFiles[fileIndex].source == NATIVE_CD_FILE_NONE))
	{
		return 0;
	}

	s_nativeCdCurrentFile = fileIndex;
	s_nativeCdCurrentSector = sector;

	if (s_nativeCdFiles[s_nativeCdCurrentFile].source == NATIVE_CD_FILE_DISC)
	{
		return 1;
	}

	return fseek(s_nativeCdFiles[s_nativeCdCurrentFile].hostFile, sector * NATIVE_CD_SECTOR_SIZE, SEEK_SET) == 0;
}

#if defined(__3DS__)
// Boot-time prefetch of a handful of disc subfiles into RAM. The music
// sequencer ticks from the per-vblank callback on the main thread, so a long
// synchronous SD read during a level load starves it and tears an audible gap.
// Reading those subfiles up front (before any music starts) lets the in-load
// reads hit RAM instead. Bounded and fail-safe: overflow, non-disc sources, and
// allocation failures skip caching and leave the disc path untouched.
#define NATIVE_CD_PREFETCH_MAX 4

struct NativeCDPrefetchExtent
{
	s32 fileIndex;
	u32 startSector;
	u32 sectorCount;
	u8 *data;
};

global_variable struct NativeCDPrefetchExtent s_cdPrefetch[NATIVE_CD_PREFETCH_MAX];
global_variable s32 s_cdPrefetchCount;

// In-flight chunked prefetch (data == NULL means idle). The extent is published
// into s_cdPrefetch only once fully read, so partial data can never serve a read.
global_variable struct NativeCDPrefetchExtent s_cdPrefetchPending;
global_variable u32 s_cdPrefetchPendingRead;

internal u8 *NativeCD_PrefetchLookup(s32 fileIndex, u32 sector, s32 sectors)
{
	s32 i;

	for (i = 0; i < s_cdPrefetchCount; i++)
	{
		struct NativeCDPrefetchExtent *slot = &s_cdPrefetch[i];
		u32 rel;

		if ((slot->fileIndex != fileIndex) || (sector < slot->startSector))
		{
			continue;
		}

		rel = sector - slot->startSector;
		if ((rel < slot->sectorCount) && ((u32)sectors <= slot->sectorCount - rel))
		{
			return slot->data + (size_t)rel * NATIVE_CD_SECTOR_SIZE;
		}
	}

	return NULL;
}
#endif

void NativeCD_PrefetchExtent(int encodedLoc, int byteSize)
{
#if defined(__3DS__)
	s32 fileIndex;
	u32 startSector;
	u32 sectorCount;
	u8 *buffer;
	struct NativeCDPrefetchExtent *slot;

	if ((byteSize <= 0) || (s_cdPrefetchCount >= NATIVE_CD_PREFETCH_MAX))
	{
		return;
	}

	fileIndex = (encodedLoc >> 24) & 0xff;
	startSector = (u32)(encodedLoc & 0xffffff);

	if ((fileIndex < 0) || (fileIndex >= s_nativeCdFileCount) || (s_nativeCdFiles[fileIndex].source != NATIVE_CD_FILE_DISC))
	{
		return;
	}

	sectorCount = ((u32)byteSize + (NATIVE_CD_SECTOR_SIZE - 1)) / NATIVE_CD_SECTOR_SIZE;

	buffer = (u8 *)malloc((size_t)sectorCount * NATIVE_CD_SECTOR_SIZE);
	if (buffer == NULL)
	{
		return;
	}

	if (!NativeDiscImage_ReadDataSectors(&s_nativeCdFiles[fileIndex].discFile, startSector, sectorCount, buffer))
	{
		free(buffer);
		return;
	}

	slot = &s_cdPrefetch[s_cdPrefetchCount++];
	slot->fileIndex = fileIndex;
	slot->startSector = startSector;
	slot->sectorCount = sectorCount;
	slot->data = buffer;
#else
	(void)encodedLoc;
	(void)byteSize;
#endif
}

int NativeCD_PrefetchExtentBegin(int encodedLoc, int byteSize)
{
#if defined(__3DS__)
	s32 fileIndex;
	u32 startSector;
	u32 sectorCount;
	u8 *buffer;

	if (s_cdPrefetchPending.data != NULL)
	{
		return 1;
	}

	if ((byteSize <= 0) || (s_cdPrefetchCount >= NATIVE_CD_PREFETCH_MAX))
	{
		return 0;
	}

	fileIndex = (encodedLoc >> 24) & 0xff;
	startSector = (u32)(encodedLoc & 0xffffff);
	sectorCount = ((u32)byteSize + (NATIVE_CD_SECTOR_SIZE - 1)) / NATIVE_CD_SECTOR_SIZE;

	if ((fileIndex < 0) || (fileIndex >= s_nativeCdFileCount) || (s_nativeCdFiles[fileIndex].source != NATIVE_CD_FILE_DISC))
	{
		return 0;
	}

	if (NativeCD_PrefetchLookup(fileIndex, startSector, (s32)sectorCount) != NULL)
	{
		return 0;
	}

	buffer = (u8 *)malloc((size_t)sectorCount * NATIVE_CD_SECTOR_SIZE);
	if (buffer == NULL)
	{
		return 0;
	}

	s_cdPrefetchPending.fileIndex = fileIndex;
	s_cdPrefetchPending.startSector = startSector;
	s_cdPrefetchPending.sectorCount = sectorCount;
	s_cdPrefetchPending.data = buffer;
	s_cdPrefetchPendingRead = 0;
	return 1;
#else
	(void)encodedLoc;
	(void)byteSize;
	return 0;
#endif
}

int NativeCD_PrefetchPump(int maxSectors)
{
#if defined(__3DS__)
	u32 chunk;

	if (s_cdPrefetchPending.data == NULL)
	{
		return 1;
	}

	chunk = s_cdPrefetchPending.sectorCount - s_cdPrefetchPendingRead;
	if ((maxSectors > 0) && ((u32)maxSectors < chunk))
	{
		chunk = (u32)maxSectors;
	}

	if (!NativeDiscImage_ReadDataSectors(&s_nativeCdFiles[s_cdPrefetchPending.fileIndex].discFile, s_cdPrefetchPending.startSector + s_cdPrefetchPendingRead, chunk,
	                                     s_cdPrefetchPending.data + (size_t)s_cdPrefetchPendingRead * NATIVE_CD_SECTOR_SIZE))
	{
		free(s_cdPrefetchPending.data);
		s_cdPrefetchPending.data = NULL;
		return 1;
	}

	s_cdPrefetchPendingRead += chunk;
	if (s_cdPrefetchPendingRead < s_cdPrefetchPending.sectorCount)
	{
		return 0;
	}

	if (s_cdPrefetchCount < NATIVE_CD_PREFETCH_MAX)
	{
		s_cdPrefetch[s_cdPrefetchCount++] = s_cdPrefetchPending;
	}
	else
	{
		free(s_cdPrefetchPending.data);
	}

	s_cdPrefetchPending.data = NULL;
	return 1;
#else
	(void)maxSectors;
	return 1;
#endif
}

int NativeCD_PrefetchExtentCached(int encodedLoc, int byteSize)
{
#if defined(__3DS__)
	s32 fileIndex;
	u32 startSector;
	u32 sectorCount;

	if (byteSize <= 0)
	{
		return 0;
	}

	fileIndex = (encodedLoc >> 24) & 0xff;
	startSector = (u32)(encodedLoc & 0xffffff);
	sectorCount = ((u32)byteSize + (NATIVE_CD_SECTOR_SIZE - 1)) / NATIVE_CD_SECTOR_SIZE;

	if ((fileIndex < 0) || (fileIndex >= s_nativeCdFileCount) || (s_nativeCdFiles[fileIndex].source != NATIVE_CD_FILE_DISC))
	{
		return 0;
	}

	return NativeCD_PrefetchLookup(fileIndex, startSector, (s32)sectorCount) != NULL;
#else
	(void)encodedLoc;
	(void)byteSize;
	return -1;
#endif
}

internal s32 NativeCD_ReadSectors(s32 sectors, void *dst)
{
	size_t byteCount;

	if ((s_nativeCdCurrentFile < 0) || (s_nativeCdCurrentFile >= s_nativeCdFileCount) || (s_nativeCdFiles[s_nativeCdCurrentFile].source == NATIVE_CD_FILE_NONE))
	{
		return 0;
	}

	byteCount = (size_t)sectors * NATIVE_CD_SECTOR_SIZE;

	if (s_nativeCdFiles[s_nativeCdCurrentFile].source == NATIVE_CD_FILE_DISC)
	{
#if defined(__3DS__)
		{
			u8 *cached = NativeCD_PrefetchLookup(s_nativeCdCurrentFile, (u32)s_nativeCdCurrentSector, sectors);
			if (cached != NULL)
			{
				memcpy(dst, cached, byteCount);
				s_nativeCdCurrentSector += sectors;
				return 1;
			}
		}
#endif
#if CTR_NATIVE_DISC_IO_PROFILE
		s_nativeDiscIoSiteCd++;
		s_nativeDiscIoSiteCdSectors += (u32)sectors;
#endif
		if (!NativeDiscImage_ReadDataSectors(&s_nativeCdFiles[s_nativeCdCurrentFile].discFile, (u32)s_nativeCdCurrentSector, (u32)sectors, dst))
		{
			return 0;
		}

		s_nativeCdCurrentSector += sectors;
		return 1;
	}

	{
		FILE *hostFile = s_nativeCdFiles[s_nativeCdCurrentFile].hostFile;
		size_t got = fread(dst, 1, byteCount, hostFile);

		if (got == byteCount)
		{
			return 1;
		}
		if ((got != 0) && !ferror(hostFile))
		{
			// The last sector of an extracted file whose size is not a whole
			// number of sectors. A disc read returns the rest of that sector,
			// which belongs to no file, so zeros stand in for it.
			memset((u8 *)dst + got, 0, byteCount - got);
			return 1;
		}

		s_nativeCdHostReadError = ferror(hostFile) ? NATIVE_CD_HOST_READ_IO_FAILED : NATIVE_CD_HOST_READ_PAST_END;
		return 0;
	}
}

internal const char *NativeCD_ReadFailureCause(void)
{
	if ((s_nativeCdCurrentFile < 0) || (s_nativeCdCurrentFile >= s_nativeCdFileCount) || (s_nativeCdFiles[s_nativeCdCurrentFile].source == NATIVE_CD_FILE_NONE))
	{
		return "no file was open for the read";
	}

	if (s_nativeCdFiles[s_nativeCdCurrentFile].source == NATIVE_CD_FILE_HOST)
	{
		return (s_nativeCdHostReadError == NATIVE_CD_HOST_READ_PAST_END) ? "the extracted file ends\nbefore this sector" : "the extracted file could not\nbe read";
	}

	switch (NativeDiscImage_LastReadError(NATIVE_DISC_IMAGE_OWNER_MAIN))
	{
	case NATIVE_DISC_IMAGE_READ_OUTSIDE_FILE:
	{
		return "the read runs past the end of\nthe file";
	}
	case NATIVE_DISC_IMAGE_READ_PAST_IMAGE_END:
	{
		return "ctr-u.bin ends before this\nsector";
	}
	case NATIVE_DISC_IMAGE_READ_BAD_SECTOR:
	{
		return "the sector is damaged in\nctr-u.bin";
	}
	default:
	{
#if defined(__3DS__)
		return "the SD card read failed";
#else
		return "the read from disk failed";
#endif
	}
	}
}

// The game answers a failed read by retrying it forever (LOAD_ReadFile_ex
// loops until CdRead succeeds; the async callback requeues on CdlDiskError),
// so a read that cannot succeed is never handed back to it. It ends the run
// on a screen that says which file and why instead.
internal void NativeCD_FailRead(s32 sectors)
{
	char message[512];
	const char *name = "(none)";

	if ((s_nativeCdCurrentFile >= 0) && (s_nativeCdCurrentFile < s_nativeCdFileCount) && (s_nativeCdFiles[s_nativeCdCurrentFile].name[0] != '\0'))
	{
		name = s_nativeCdFiles[s_nativeCdCurrentFile].name;
	}

	snprintf(message, sizeof(message),
	         "Could not read the game data.\n\n"
	         "File: %s\n"
	         "Sectors: %d to %d\n"
	         "Cause: %s.\n\n"
	         "The disc image is damaged or incomplete.\n"
	         "Copy the .bin again and check that its\n"
	         "size matches the original.",
	         name, (int)s_nativeCdCurrentSector, (int)(s_nativeCdCurrentSector + sectors - 1), NativeCD_ReadFailureCause());
	NativePlatform_FatalExit(message);
}

internal void NativeCD_SetLastCom(int com)
{
	s_cdLastCom = com;
}

int NativeCD_Init(void)
{
	s32 i;

	for (i = 0; i < s_nativeCdFileCount; i++)
	{
		if (s_nativeCdFiles[i].hostFile != NULL)
		{
			fclose(s_nativeCdFiles[i].hostFile);
		}
	}

	s_cdDebugLevel = 0;
	s_cdLastCom = 0;
	s_cdReadyCallback = NULL;
	s_cdSyncCallback = NULL;
	s_nativeCdReadCallback = NULL;
	memset(s_nativeCdFiles, 0, sizeof(s_nativeCdFiles));
	s_nativeCdFileCount = 0;
	s_nativeCdCurrentFile = -1;
	s_nativeCdCurrentSector = 0;
	memset(s_cdSectorData, 0, sizeof(s_cdSectorData));
	return 0;
}

int CdInit(void)
{
	s_cdLastCom = 0;
	s_nativeCdCurrentFile = -1;
	s_nativeCdCurrentSector = 0;
	memset(s_cdSectorData, 0, sizeof(s_cdSectorData));
	return 1;
}

int CdSetDebug(int level)
{
	s32 old = s_cdDebugLevel;
	s_cdDebugLevel = level;
	return old;
}

int CdLastCom(void)
{
	return s_cdLastCom;
}

CdlCB CdReadyCallback(CdlCB func)
{
	CdlCB old = s_cdReadyCallback;
	s_cdReadyCallback = func;
	return old;
}

CdlCB CdSyncCallback(CdlCB func)
{
	CdlCB old = s_cdSyncCallback;
	s_cdSyncCallback = func;
	return old;
}

int CdGetSector(void *madr, int size)
{
	u32 byteCount;

	if ((madr == NULL) || (size <= 0))
	{
		return 1;
	}

	byteCount = (u32)size * sizeof(u32);
	if (byteCount > sizeof(s_cdSectorData))
	{
		byteCount = sizeof(s_cdSectorData);
	}

	memcpy(madr, s_cdSectorData, byteCount);
	return 1;
}

CdlCB CdReadCallback(CdlCB func)
{
	CdlCB old = s_nativeCdReadCallback;
	s_nativeCdReadCallback = func;
	return old;
}

int CdPosToInt(CdlLOC *p)
{
	return NativeCD_PosToInt(p);
}

CdlLOC *CdIntToPos(int val, CdlLOC *p)
{
	NativeCD_IntToPos(val, p);
	return p;
}

CdlFILE *CdSearchFile(CdlFILE *loc, char *filename)
{
	if (NativeCD_SearchFile(loc, filename) == 0)
	{
		return NULL;
	}

	return loc;
}

int CdControl(uint8_t com, uint8_t *param, uint8_t *result)
{
	(void)result;

	NativeCD_SetLastCom(com);

	if ((com == CdlSetloc) && (param != NULL))
	{
		NativeCD_SetLoc((const CdlLOC *)param);
	}

	if ((com == CdlSetmode) && (param != NULL))
	{
		if (param[0] == 0xE8)
		{
			boolDecodeXaDuringVsyncCallback = 1;
		}

		if (param[0] == CdlModeSpeed)
		{
			boolDecodeXaDuringVsyncCallback = 0;
		}
	}

	return 1;
}

int CdRead(int sectors, uint32_t *buf, int mode)
{
	(void)mode;

	if (!NativeCD_ReadSectors(sectors, buf))
	{
		// One retry for a transient SD error. Only the disc path leaves the
		// read position where it was on failure; a host stream has moved.
		if ((s_nativeCdCurrentFile < 0) || (s_nativeCdCurrentFile >= s_nativeCdFileCount) || (s_nativeCdFiles[s_nativeCdCurrentFile].source != NATIVE_CD_FILE_DISC) ||
		    !NativeCD_ReadSectors(sectors, buf))
		{
			NativeCD_FailRead(sectors);
		}
	}

	if (s_nativeCdReadCallback != 0)
	{
		s_nativeCdReadCallback(CdlComplete, 0);
	}

	return 1;
}

int CdReadSync(int mode, uint8_t *result)
{
	(void)mode;
	(void)result;

	return 0;
}
