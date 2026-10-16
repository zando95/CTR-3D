#include "platform/native_assets.h"

#include <macros.h>

#include <platform/native_disc_image.h>
#include <platform/native_path.h>

#if defined(_WIN32)
#include <platform/native_win32.h>
#else
#include <dirent.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_ASSETS_PATH_MAX                   1024
#define NATIVE_ASSETS_DIR_NAME                   "assets"
#define NATIVE_ASSETS_BIGFILE_PATH               "BIGFILE.BIG"
#define NATIVE_ASSETS_KART_HWL_PATH              "SOUNDS/KART.HWL"
#define NATIVE_ASSETS_TEST_STR_PATH              "TEST.STR"
#define NATIVE_ASSETS_XNF_PATH                   "XA/ENG.XNF"
#define NATIVE_ASSETS_DISC_PATH                  "ctr-u.bin"

#define NATIVE_ASSETS_XA_TYPE_COUNT              3
#define NATIVE_ASSETS_XA_HEADER_SIZE             0x44
#define NATIVE_ASSETS_XA_NUM_XAS_TOTAL_OFFSET    0x0c
#define NATIVE_ASSETS_XA_NUM_TRACKS_TOTAL_OFFSET 0x10
#define NATIVE_ASSETS_XA_NUM_SONGS_OFFSET        0x2c
#define NATIVE_ASSETS_XA_FIRST_SONG_INDEX_OFFSET 0x38
#define NATIVE_ASSETS_XA_ENTRY_BYTES             4
#define NATIVE_ASSETS_XA_MAX_FILE_NUMBER         256

struct NativeAssetIndexEntry
{
	char *key;
	char *path;
};

global_variable char s_nativeAssetsBaseDir[NATIVE_ASSETS_PATH_MAX] = ".";
global_variable char s_nativeAssetsDir[NATIVE_ASSETS_PATH_MAX] = NATIVE_ASSETS_DIR_NAME;
global_variable int s_nativeAssetsInitialized;
global_variable struct NativeAssetIndexEntry *s_nativeAssetIndex;
global_variable int s_nativeAssetIndexCount;
global_variable int s_nativeAssetIndexCapacity;
global_variable int s_nativeAssetIndexBuilt;

internal int NativeAssets_FileExistsHost(const char *path)
{
	FILE *file;

	if (path == NULL)
	{
		return 0;
	}

	file = fopen(path, "rb");
	if (file == NULL)
	{
		return 0;
	}

	fclose(file);
	return 1;
}

internal int NativeAssets_DirectoryExistsHost(const char *path)
{
#if defined(_WIN32)
	DWORD attributes;

	if (path == NULL)
		return 0;

	attributes = GetFileAttributesA(path);
	return (attributes != INVALID_FILE_ATTRIBUTES) && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
	DIR *dir = opendir(path);

	if (dir == NULL)
	{
		return 0;
	}

	closedir(dir);
	return 1;
#endif
}

internal int NativeAssets_FindHostChildCaseInsensitive(NativeStr8 parent, NativeStr8 child, char *dst, size_t dstSize)
{
	char parentPath[NATIVE_ASSETS_PATH_MAX];
	char match[NATIVE_ASSETS_PATH_MAX];

	if (!NativePath_NormalizeSlashes(parentPath, sizeof(parentPath), parent))
	{
		return 0;
	}

#if defined(_WIN32)
	{
		char searchPath[NATIVE_ASSETS_PATH_MAX];
		WIN32_FIND_DATAA findData;
		HANDLE findHandle;

		if (!NativePath_Join(searchPath, sizeof(searchPath), NativeStr8_FromCString(parentPath), NATIVE_STR8_LIT("*")))
			return 0;

		findHandle = FindFirstFileA(searchPath, &findData);
		if (findHandle == INVALID_HANDLE_VALUE)
			return 0;

		match[0] = '\0';
		do
		{
			NativeStr8 entryName = NativeStr8_FromCString(findData.cFileName);

			if (NativeStr8_Equals(entryName, child))
			{
				if (!NativeStr8_CopyToCString(match, sizeof(match), entryName))
				{
					FindClose(findHandle);
					return 0;
				}

				break;
			}

			if ((match[0] == '\0') && NativeStr8_EqualsIgnoreCaseAscii(entryName, child))
			{
				if (!NativeStr8_CopyToCString(match, sizeof(match), entryName))
				{
					FindClose(findHandle);
					return 0;
				}
			}
		} while (FindNextFileA(findHandle, &findData) != 0);

		FindClose(findHandle);
	}
#else
	DIR *dir;
	struct dirent *entry;

	dir = opendir(parentPath);
	if (dir == NULL)
	{
		return 0;
	}

	match[0] = '\0';
	while ((entry = readdir(dir)) != NULL)
	{
		NativeStr8 entryName = NativeStr8_FromCString(entry->d_name);

		if (NativeStr8_Equals(entryName, child))
		{
			if (!NativeStr8_CopyToCString(match, sizeof(match), entryName))
			{
				closedir(dir);
				return 0;
			}

			break;
		}

		if ((match[0] == '\0') && NativeStr8_EqualsIgnoreCaseAscii(entryName, child))
		{
			if (!NativeStr8_CopyToCString(match, sizeof(match), entryName))
			{
				closedir(dir);
				return 0;
			}
		}
	}

	closedir(dir);
#endif

	if (match[0] == '\0')
	{
		return 0;
	}

	return NativePath_Join(dst, dstSize, NativeStr8_FromCString(parentPath), NativeStr8_FromCString(match));
}

internal int NativeAssets_FindAssetsDir(NativeStr8 baseDir, char *dst, size_t dstSize)
{
	if (!NativePath_Join(dst, dstSize, baseDir, NATIVE_STR8_LIT(NATIVE_ASSETS_DIR_NAME)))
	{
		return 0;
	}

	if (NativeAssets_DirectoryExistsHost(dst))
	{
		return 1;
	}

	return NativeAssets_FindHostChildCaseInsensitive(baseDir, NATIVE_STR8_LIT(NATIVE_ASSETS_DIR_NAME), dst, dstSize);
}

internal char *NativeAssets_CopyCString(const char *src)
{
	size_t size;
	char *copy;

	if (src == NULL)
	{
		return NULL;
	}

	size = strlen(src) + 1u;
	copy = (char *)malloc(size);
	if (copy == NULL)
	{
		return NULL;
	}

	memcpy(copy, src, size);
	return copy;
}

internal void NativeAssets_ClearIndex(void)
{
	int i;

	for (i = 0; i < s_nativeAssetIndexCount; i++)
	{
		free(s_nativeAssetIndex[i].key);
		free(s_nativeAssetIndex[i].path);
	}

	free(s_nativeAssetIndex);
	s_nativeAssetIndex = NULL;
	s_nativeAssetIndexCount = 0;
	s_nativeAssetIndexCapacity = 0;
	s_nativeAssetIndexBuilt = 0;
}

internal int NativeAssets_IndexReserve(int needed)
{
	struct NativeAssetIndexEntry *entries;
	int newCapacity;

	if (needed <= s_nativeAssetIndexCapacity)
	{
		return 1;
	}

	newCapacity = (s_nativeAssetIndexCapacity == 0) ? 256 : s_nativeAssetIndexCapacity * 2;
	while (newCapacity < needed)
	{
		newCapacity *= 2;
	}

	entries = (struct NativeAssetIndexEntry *)realloc(s_nativeAssetIndex, (size_t)newCapacity * sizeof(*s_nativeAssetIndex));
	if (entries == NULL)
	{
		return 0;
	}

	s_nativeAssetIndex = entries;
	s_nativeAssetIndexCapacity = newCapacity;
	return 1;
}

internal int NativeAssets_NormalizeRelativeKey(NativeStr8 relativePath, char *dst, size_t dstSize)
{
	size_t i;

	relativePath = NativePath_SkipLeadingSeparators(relativePath);
	if ((dst == NULL) || (dstSize == 0) || (relativePath.len >= dstSize))
	{
		return 0;
	}

	for (i = 0; i < relativePath.len; i++)
	{
		u8 byte = relativePath.ptr[i];
		dst[i] = (char)(NativePath_IsSeparator(byte) ? '/' : NativeStr8_ToLowerAscii(byte));
	}

	dst[relativePath.len] = '\0';
	return 1;
}

internal const char *NativeAssets_FindIndexedPath(NativeStr8 relativePath)
{
	char key[NATIVE_ASSETS_PATH_MAX];
	int i;

	if (!NativeAssets_NormalizeRelativeKey(relativePath, key, sizeof(key)))
	{
		return NULL;
	}

	for (i = 0; i < s_nativeAssetIndexCount; i++)
	{
		if (strcmp(s_nativeAssetIndex[i].key, key) == 0)
		{
			return s_nativeAssetIndex[i].path;
		}
	}

	return NULL;
}

internal int NativeAssets_IndexAdd(NativeStr8 relativePath, const char *hostPath)
{
	char key[NATIVE_ASSETS_PATH_MAX];
	char *keyCopy;
	char *pathCopy;

	if (!NativeAssets_NormalizeRelativeKey(relativePath, key, sizeof(key)))
	{
		return 0;
	}

	if (!NativeAssets_IndexReserve(s_nativeAssetIndexCount + 1))
	{
		return 0;
	}

	keyCopy = NativeAssets_CopyCString(key);
	pathCopy = NativeAssets_CopyCString(hostPath);
	if ((keyCopy == NULL) || (pathCopy == NULL))
	{
		free(keyCopy);
		free(pathCopy);
		return 0;
	}

	s_nativeAssetIndex[s_nativeAssetIndexCount].key = keyCopy;
	s_nativeAssetIndex[s_nativeAssetIndexCount].path = pathCopy;
	s_nativeAssetIndexCount++;
	return 1;
}

internal void NativeAssets_IndexScanDir(const char *dirPath, NativeStr8 relativeDir, int depth)
{
	if (depth > 16)
	{
		return;
	}

#if defined(_WIN32)
	{
		char searchPath[NATIVE_ASSETS_PATH_MAX];
		WIN32_FIND_DATAA findData;
		HANDLE findHandle;

		if (!NativePath_Join(searchPath, sizeof(searchPath), NativeStr8_FromCString(dirPath), NATIVE_STR8_LIT("*")))
			return;

		findHandle = FindFirstFileA(searchPath, &findData);
		if (findHandle == INVALID_HANDLE_VALUE)
			return;

		do
		{
			NativeStr8 entryName = NativeStr8_FromCString(findData.cFileName);
			char childPath[NATIVE_ASSETS_PATH_MAX];
			char relativePath[NATIVE_ASSETS_PATH_MAX];

			if (NativeStr8_Equals(entryName, NATIVE_STR8_LIT(".")) || NativeStr8_Equals(entryName, NATIVE_STR8_LIT("..")))
				continue;

			if (!NativePath_Join(childPath, sizeof(childPath), NativeStr8_FromCString(dirPath), entryName))
				continue;

			if (relativeDir.len == 0)
			{
				if (!NativePath_NormalizeSlashes(relativePath, sizeof(relativePath), entryName))
					continue;
			}
			else
			{
				if (!NativePath_Join(relativePath, sizeof(relativePath), relativeDir, entryName))
					continue;
			}

			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				NativeAssets_IndexScanDir(childPath, NativeStr8_FromCString(relativePath), depth + 1);
				continue;
			}

			if (NativeAssets_FileExistsHost(childPath))
				NativeAssets_IndexAdd(NativeStr8_FromCString(relativePath), childPath);
		} while (FindNextFileA(findHandle, &findData) != 0);

		FindClose(findHandle);
	}
#else
	DIR *dir;
	struct dirent *entry;

	dir = opendir(dirPath);
	if (dir == NULL)
	{
		return;
	}

	while ((entry = readdir(dir)) != NULL)
	{
		NativeStr8 entryName = NativeStr8_FromCString(entry->d_name);
		char childPath[NATIVE_ASSETS_PATH_MAX];
		char relativePath[NATIVE_ASSETS_PATH_MAX];
		DIR *childDir;

		if (NativeStr8_Equals(entryName, NATIVE_STR8_LIT(".")) || NativeStr8_Equals(entryName, NATIVE_STR8_LIT("..")))
		{
			continue;
		}

		if (!NativePath_Join(childPath, sizeof(childPath), NativeStr8_FromCString(dirPath), entryName))
		{
			continue;
		}

		if (relativeDir.len == 0)
		{
			if (!NativePath_NormalizeSlashes(relativePath, sizeof(relativePath), entryName))
			{
				continue;
			}
		}
		else
		{
			if (!NativePath_Join(relativePath, sizeof(relativePath), relativeDir, entryName))
			{
				continue;
			}
		}

		childDir = opendir(childPath);
		if (childDir != NULL)
		{
			closedir(childDir);
			NativeAssets_IndexScanDir(childPath, NativeStr8_FromCString(relativePath), depth + 1);
			continue;
		}

		if (NativeAssets_FileExistsHost(childPath))
		{
			NativeAssets_IndexAdd(NativeStr8_FromCString(relativePath), childPath);
		}
	}

	closedir(dir);
#endif
}

internal void NativeAssets_BuildIndex(void)
{
	if (s_nativeAssetIndexBuilt)
	{
		return;
	}

	s_nativeAssetIndexBuilt = 1;

	NativeAssets_IndexScanDir(s_nativeAssetsDir, (NativeStr8){0}, 0);
}

internal int NativeAssets_BaseHasRequiredFile(NativeStr8 baseDir)
{
	char assetsDir[NATIVE_ASSETS_PATH_MAX];
	char path[NATIVE_ASSETS_PATH_MAX];

	if (!NativeAssets_FindAssetsDir(baseDir, assetsDir, sizeof(assetsDir)))
	{
		return 0;
	}

	if (NativeAssets_FindHostChildCaseInsensitive(NativeStr8_FromCString(assetsDir), NATIVE_STR8_LIT(NATIVE_ASSETS_BIGFILE_PATH), path, sizeof(path)))
	{
		return NativeAssets_FileExistsHost(path);
	}

	if (!NativePath_Join(path, sizeof(path), NativeStr8_FromCString(assetsDir), NATIVE_STR8_LIT(NATIVE_ASSETS_BIGFILE_PATH)))
	{
		return 0;
	}

	if (NativeAssets_FileExistsHost(path))
	{
		return 1;
	}

	if (NativeAssets_FindHostChildCaseInsensitive(NativeStr8_FromCString(assetsDir), NATIVE_STR8_LIT(NATIVE_ASSETS_DISC_PATH), path, sizeof(path)))
	{
		return NativeAssets_FileExistsHost(path);
	}

	if (!NativePath_Join(path, sizeof(path), NativeStr8_FromCString(assetsDir), NATIVE_STR8_LIT(NATIVE_ASSETS_DISC_PATH)))
	{
		return 0;
	}

	return NativeAssets_FileExistsHost(path);
}

internal int NativeAssets_SetBaseDir(NativeStr8 baseDir)
{
	char assetsDir[NATIVE_ASSETS_PATH_MAX];

	baseDir = NativePath_TrimTrailingSeparators(baseDir);
	if (!NativePath_NormalizeSlashes(s_nativeAssetsBaseDir, sizeof(s_nativeAssetsBaseDir), baseDir))
	{
		return 0;
	}

	if (!NativeAssets_FindAssetsDir(NativeStr8_FromCString(s_nativeAssetsBaseDir), assetsDir, sizeof(assetsDir)))
	{
		if (!NativePath_Join(assetsDir, sizeof(assetsDir), NativeStr8_FromCString(s_nativeAssetsBaseDir), NATIVE_STR8_LIT(NATIVE_ASSETS_DIR_NAME)))
		{
			return 0;
		}
	}

	if (!NativePath_NormalizeSlashes(s_nativeAssetsDir, sizeof(s_nativeAssetsDir), NativeStr8_FromCString(assetsDir)))
	{
		return 0;
	}

	NativeDiscImage_Init(s_nativeAssetsDir);
	NativeAssets_ClearIndex();
	s_nativeAssetsInitialized = 1;
	return 1;
}

int NativeAssets_Init(const char *executableBasePath)
{
	char parentDir[NATIVE_ASSETS_PATH_MAX];
	NativeStr8 exeDir;

	if ((executableBasePath == NULL) || (executableBasePath[0] == '\0'))
	{
		executableBasePath = ".";
	}

	exeDir = NativePath_TrimTrailingSeparators(NativeStr8_FromCString(executableBasePath));

	if (NativeAssets_BaseHasRequiredFile(exeDir))
	{
		return NativeAssets_SetBaseDir(exeDir);
	}

	if (NativePath_Parent(parentDir, sizeof(parentDir), exeDir))
	{
		NativeStr8 parent = NativeStr8_FromCString(parentDir);

		if (NativeAssets_BaseHasRequiredFile(parent))
		{
			return NativeAssets_SetBaseDir(parent);
		}
	}

	return NativeAssets_SetBaseDir(exeDir);
}

const char *NativeAssets_GetBaseDir(void)
{
	return s_nativeAssetsBaseDir;
}

const char *NativeAssets_GetAssetDir(void)
{
	return s_nativeAssetsDir;
}

int NativeAssets_BuildPathStr8(NativeStr8 relativePath, char *dst, size_t dstSize)
{
	if (!s_nativeAssetsInitialized && !NativeAssets_Init("."))
	{
		return 0;
	}

	return NativePath_Join(dst, dstSize, NativeStr8_FromCString(s_nativeAssetsDir), relativePath);
}

int NativeAssets_BuildPath(const char *relativePath, char *dst, size_t dstSize)
{
	return NativeAssets_BuildPathStr8(NativeStr8_FromCString(relativePath), dst, dstSize);
}

int NativeAssets_ResolvePathStr8(NativeStr8 relativePath, char *dst, size_t dstSize)
{
	char path[NATIVE_ASSETS_PATH_MAX];
	const char *indexedPath;

	if (!NativeAssets_BuildPathStr8(relativePath, path, sizeof(path)))
	{
		return 0;
	}

	if (NativeAssets_FileExistsHost(path))
	{
		return NativePath_NormalizeSlashes(dst, dstSize, NativeStr8_FromCString(path));
	}

	NativeAssets_BuildIndex();
	indexedPath = NativeAssets_FindIndexedPath(relativePath);
	if (indexedPath == NULL)
	{
		return 0;
	}

	return NativePath_NormalizeSlashes(dst, dstSize, NativeStr8_FromCString(indexedPath));
}

int NativeAssets_ResolvePath(const char *relativePath, char *dst, size_t dstSize)
{
	return NativeAssets_ResolvePathStr8(NativeStr8_FromCString(relativePath), dst, dstSize);
}

FILE *NativeAssets_OpenHostStr8(NativeStr8 relativePath, const char *mode)
{
	char path[NATIVE_ASSETS_PATH_MAX];
	FILE *file;

	if ((mode == NULL) || !NativeAssets_BuildPathStr8(relativePath, path, sizeof(path)))
	{
		return NULL;
	}

	file = fopen(path, mode);
	if (file != NULL)
	{
		return file;
	}

	if (mode[0] != 'r')
	{
		return NULL;
	}

	if (!NativeAssets_ResolvePathStr8(relativePath, path, sizeof(path)))
	{
		return NULL;
	}

	return fopen(path, mode);
}

FILE *NativeAssets_OpenHost(const char *relativePath, const char *mode)
{
	return NativeAssets_OpenHostStr8(NativeStr8_FromCString(relativePath), mode);
}

FILE *NativeAssets_OpenHostBigfile(const char *mode)
{
	return NativeAssets_OpenHostStr8(NATIVE_STR8_LIT(NATIVE_ASSETS_BIGFILE_PATH), mode);
}

internal int NativeAssets_ReadExact(FILE *file, void *dst, size_t size)
{
	return fread(dst, 1, size, file) == size;
}

internal u32 NativeAssets_ReadLE32(const u8 *data)
{
	return ((u32)data[0]) | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

internal int NativeAssets_ReadHostBytes(const char *path, struct NativeAssetsByteBuffer *bytes)
{
	FILE *file;
	long size;

	bytes->data = NULL;
	bytes->size = 0;

	file = NativeAssets_OpenHost(path, "rb");
	if (file == NULL)
	{
		return 0;
	}

	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return 0;
	}

	size = ftell(file);
	if ((size <= 0) || (size > 0x7fffffff))
	{
		fclose(file);
		return 0;
	}

	if (fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return 0;
	}

	bytes->data = (u8 *)malloc((size_t)size);
	if (bytes->data == NULL)
	{
		fclose(file);
		return 0;
	}

	if (!NativeAssets_ReadExact(file, bytes->data, (size_t)size))
	{
		free(bytes->data);
		bytes->data = NULL;
		fclose(file);
		return 0;
	}

	fclose(file);
	bytes->size = (int)size;
	return 1;
}

int NativeAssets_OpenHostRange(const char *relativePath, struct NativeAssetsHostRange *range)
{
	FILE *file;
	long size;

	range->file = NULL;
	range->size = 0;

	file = NativeAssets_OpenHost(relativePath, "rb");
	if (file == NULL)
	{
		return 0;
	}

	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return 0;
	}

	size = ftell(file);
	if ((size <= 0) || (size > 0x7fffffff))
	{
		fclose(file);
		return 0;
	}

	if (fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return 0;
	}

	range->file = file;
	range->size = size;
	return 1;
}

int NativeAssets_ReadHostRange(struct NativeAssetsHostRange *range, long offset, void *dst, size_t size)
{
	if ((range == NULL) || (range->file == NULL) || (dst == NULL) || (offset < 0) || (size == 0) || (size > 0x7fffffffu))
	{
		return 0;
	}

	if ((range->size > 0) && (offset > range->size - (long)size))
	{
		return 0;
	}

	if (fseek(range->file, offset, SEEK_SET) != 0)
	{
		return 0;
	}

	return NativeAssets_ReadExact(range->file, dst, size);
}

void NativeAssets_CloseHostRange(struct NativeAssetsHostRange *range)
{
	if (range == NULL)
	{
		return;
	}

	if (range->file != NULL)
	{
		fclose(range->file);
	}
	range->file = NULL;
	range->size = 0;
}

int NativeAssets_ReadBytes(const char *path, int readMode, struct NativeAssetsByteBuffer *bytes)
{
	bytes->data = NULL;
	bytes->size = 0;

	if (NativeAssets_ReadHostBytes(path, bytes))
	{
		return 1;
	}

	return NativeDiscImage_ReadFileBytes(path, readMode == NATIVE_ASSET_READ_RAW_CD_SECTORS, &bytes->data, &bytes->size);
}

void NativeAssets_FreeBytes(struct NativeAssetsByteBuffer *bytes)
{
	free(bytes->data);
	bytes->data = NULL;
	bytes->size = 0;
}

/*
 * Startup failure reporting. Validate leaves one player-facing explanation in
 * s_nativeAssetsFailure for the platform's fatal screen. Lines stay within 44
 * columns so the 3DS console (50 columns, indented 2) never wraps mid-word.
 */
#define NATIVE_ASSETS_FAILURE_MAX 1536
#define NATIVE_ASSETS_RETAIL_BOOT "SCUS_944.26"

enum NativeAssetsImageHint
{
	NATIVE_ASSETS_HINT_NONE,
	NATIVE_ASSETS_HINT_CUE,
	NATIVE_ASSETS_HINT_COMPRESSED,
	NATIVE_ASSETS_HINT_ISO,
	NATIVE_ASSETS_HINT_RAW_OUTSIDE,
	NATIVE_ASSETS_HINT_RAW,
	NATIVE_ASSETS_HINT_MISPLACED,
};

global_variable char s_nativeAssetsFailure[NATIVE_ASSETS_FAILURE_MAX];

internal void NativeAssets_SetFailure(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
internal void NativeAssets_SetFailure(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(s_nativeAssetsFailure, sizeof(s_nativeAssetsFailure), fmt, args);
	va_end(args);
}

const char *NativeAssets_GetFailureMessage(void)
{
	if (s_nativeAssetsFailure[0] == '\0')
	{
		return "Could not set up the game's asset folder.";
	}

	return s_nativeAssetsFailure;
}

internal void NativeAssets_PrintHeader(void)
{
	fprintf(stderr, "[CTR 3D] Missing or incomplete assets.\n");
	fprintf(stderr, "[CTR 3D] Expected NTSC-U retail assets under: %s\n", NativeAssets_GetAssetDir());
}

internal void NativeAssets_PrintFooter(void)
{
	fprintf(stderr, "[CTR 3D] Provide either raw NTSC-U disc image %s, or extracted files:\n", NATIVE_ASSETS_DISC_PATH);
	fprintf(stderr, "[CTR 3D]   %s, %s, %s, %s, plus XA files referenced by %s\n", NATIVE_ASSETS_BIGFILE_PATH, NATIVE_ASSETS_KART_HWL_PATH,
	        NATIVE_ASSETS_TEST_STR_PATH, NATIVE_ASSETS_XNF_PATH, NATIVE_ASSETS_XNF_PATH);
}

internal int NativeAssets_NameHasExtension(const char *name, const char *ext)
{
	NativeStr8 nameStr = NativeStr8_FromCString(name);
	NativeStr8 extStr = NativeStr8_FromCString(ext);

	return (nameStr.len > extStr.len) && NativeStr8_EqualsIgnoreCaseAscii(NativeStr8_Skip(nameStr, nameStr.len - extStr.len), extStr);
}

internal int NativeAssets_ClassifyImageName(const char *name, int inAssetsDir)
{
	if (NativeStr8_EqualsIgnoreCaseAscii(NativeStr8_FromCString(name), NATIVE_STR8_LIT(NATIVE_ASSETS_DISC_PATH)))
	{
		return inAssetsDir ? NATIVE_ASSETS_HINT_NONE : NATIVE_ASSETS_HINT_MISPLACED;
	}
	if (NativeAssets_NameHasExtension(name, ".bin") || NativeAssets_NameHasExtension(name, ".img"))
	{
		return inAssetsDir ? NATIVE_ASSETS_HINT_RAW : NATIVE_ASSETS_HINT_RAW_OUTSIDE;
	}
	if (NativeAssets_NameHasExtension(name, ".iso"))
	{
		return NATIVE_ASSETS_HINT_ISO;
	}
	if (NativeAssets_NameHasExtension(name, ".chd") || NativeAssets_NameHasExtension(name, ".pbp") || NativeAssets_NameHasExtension(name, ".ecm") ||
	    NativeAssets_NameHasExtension(name, ".zip") || NativeAssets_NameHasExtension(name, ".7z") || NativeAssets_NameHasExtension(name, ".rar"))
	{
		return NATIVE_ASSETS_HINT_COMPRESSED;
	}
	if (NativeAssets_NameHasExtension(name, ".cue"))
	{
		return NATIVE_ASSETS_HINT_CUE;
	}

	return NATIVE_ASSETS_HINT_NONE;
}

internal void NativeAssets_ConsiderImageName(const char *name, int inAssetsDir, int *bestHint, char *bestName, size_t bestNameSize)
{
	int hint = NativeAssets_ClassifyImageName(name, inAssetsDir);

	if (hint > *bestHint)
	{
		*bestHint = hint;
		snprintf(bestName, bestNameSize, "%s", name);
	}
}

/* The likeliest thing the player meant as the disc image, from one directory:
 * a misplaced ctr-u.bin beats a misnamed raw dump beats an .iso, and so on. */
internal void NativeAssets_ScanForImageHint(const char *dirPath, int inAssetsDir, int *bestHint, char *bestName, size_t bestNameSize)
{
#if defined(_WIN32)
	char searchPath[NATIVE_ASSETS_PATH_MAX];
	WIN32_FIND_DATAA findData;
	HANDLE findHandle;

	if (!NativePath_Join(searchPath, sizeof(searchPath), NativeStr8_FromCString(dirPath), NATIVE_STR8_LIT("*")))
		return;

	findHandle = FindFirstFileA(searchPath, &findData);
	if (findHandle == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			NativeAssets_ConsiderImageName(findData.cFileName, inAssetsDir, bestHint, bestName, bestNameSize);
	} while (FindNextFileA(findHandle, &findData) != 0);

	FindClose(findHandle);
#else
	DIR *dir = opendir(dirPath);
	struct dirent *entry;

	if (dir == NULL)
	{
		return;
	}

	while ((entry = readdir(dir)) != NULL)
	{
		NativeAssets_ConsiderImageName(entry->d_name, inAssetsDir, bestHint, bestName, bestNameSize);
	}

	closedir(dir);
#endif
}

internal void NativeAssets_FailNotFound(void)
{
	char hintName[260]; /* Win32 MAX_PATH, the widest entry name either scan returns */
	char hint[512];
	int hintKind = NATIVE_ASSETS_HINT_NONE;

	hintName[0] = '\0';
	NativeAssets_ScanForImageHint(s_nativeAssetsDir, 1, &hintKind, hintName, sizeof(hintName));
	NativeAssets_ScanForImageHint(s_nativeAssetsBaseDir, 0, &hintKind, hintName, sizeof(hintName));

	switch (hintKind)
	{
	case NATIVE_ASSETS_HINT_MISPLACED:
	{
		snprintf(hint, sizeof(hint), "Found %s outside the assets\nfolder. Move it into the assets folder.\n\n", hintName);
		break;
	}
	case NATIVE_ASSETS_HINT_RAW:
	{
		snprintf(hint, sizeof(hint), "Found %s. If that is your\nCTR dump, rename it to ctr-u.bin.\n\n", hintName);
		break;
	}
	case NATIVE_ASSETS_HINT_RAW_OUTSIDE:
	{
		snprintf(hint, sizeof(hint), "Found %s outside the assets\nfolder. If that is your CTR dump, move it\ninto the assets folder as ctr-u.bin.\n\n",
		         hintName);
		break;
	}
	case NATIVE_ASSETS_HINT_ISO:
	{
		snprintf(hint, sizeof(hint),
		         "Found %s, but a 2048-byte .iso\nwill not work: it drops the sectors the\nmusic and FMV are stored in. Dump the\n"
		         "disc again as .bin/.cue.\n\n",
		         hintName);
		break;
	}
	case NATIVE_ASSETS_HINT_COMPRESSED:
	{
		snprintf(hint, sizeof(hint), "Found %s. Compressed images\n(.chd, .pbp, archives) will not work.\nConvert it back to .bin/.cue first.\n\n",
		         hintName);
		break;
	}
	case NATIVE_ASSETS_HINT_CUE:
	{
		snprintf(hint, sizeof(hint), "Found %s. The .cue only\ndescribes the dump: copy the .bin it\nlists, renamed to ctr-u.bin.\n\n", hintName);
		break;
	}
	default:
	{
		snprintf(hint, sizeof(hint), "A 2048-byte .iso will not work: it drops\nthe sectors the music and FMV are stored\nin.\n\n");
		break;
	}
	}

	NativeAssets_SetFailure("Could not find the game's disc image.\n\n"
	                        "This port ships no game data. Copy your\n"
	                        "own NTSC-U Crash Team Racing disc dump\n"
#if defined(__3DS__)
	                        "to the SD card as:\n\n"
	                        "  /3ds/ctr_native/assets/ctr-u.bin\n\n"
#else
	                        "into the assets folder as:\n\n"
	                        "  assets/ctr-u.bin\n\n"
#endif
	                        "It must be a raw MODE2/2352 .bin dump.\n"
	                        "%s"
	                        "Looked in: %s",
	                        hint, s_nativeAssetsDir);
}

internal void NativeAssets_FailTruncated(const char *what, u64 haveBytes, u64 needBytes)
{
	NativeAssets_SetFailure("The game data is incomplete.\n\n"
	                        "%s is %llu bytes\n"
	                        "but needs at least %llu.\n\n"
	                        "The copy was cut short. Copy the .bin\n"
	                        "again and check that the sizes match.",
	                        what, (unsigned long long)haveBytes, (unsigned long long)needBytes);
}

internal void NativeAssets_FailDiscImage(const struct NativeDiscImageInfo *disc)
{
	switch (disc->status)
	{
	case NATIVE_DISC_IMAGE_STATUS_OPEN_FAILED:
	{
		NativeAssets_SetFailure("Found the disc image but could not open\nit:\n\n  %s\n\nCopy it again; the file may be damaged.", disc->path);
		break;
	}
	case NATIVE_DISC_IMAGE_STATUS_COOKED_ISO:
	{
		NativeAssets_SetFailure("The disc image is a 2048-byte .iso.\n\n"
		                        "ctr-u.bin has 2048-byte sectors, so the\n"
		                        "sectors the music and FMV are stored in\n"
		                        "are missing. Dump the disc again as a\n"
		                        "raw MODE2/2352 .bin/.cue and copy the\n"
		                        ".bin as ctr-u.bin.");
		break;
	}
	case NATIVE_DISC_IMAGE_STATUS_NO_VOLUME:
	{
		NativeAssets_SetFailure("The disc image is not a PlayStation dump.\n\n"
		                        "ctr-u.bin has no disc header in the raw\n"
		                        "MODE2/2352 layout. Use the .bin from a\n"
		                        ".bin/.cue dump, not a .cue, .chd, .pbp or\n"
		                        "archive renamed to .bin.");
		break;
	}
	case NATIVE_DISC_IMAGE_STATUS_TRUNCATED:
	{
		NativeAssets_FailTruncated("ctr-u.bin", disc->imageBytes, (u64)disc->volumeSectors * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE);
		break;
	}
	default:
	{
		NativeAssets_SetFailure("Could not read the disc image.");
		break;
	}
	}
}

/* "SCES_021.05" -> "SCES-02105", the form printed on the disc. */
internal void NativeAssets_FormatSerial(char *dst, size_t dstSize, const char *bootName)
{
	size_t out = 0;
	size_t i;

	for (i = 0; (bootName[i] != '\0') && (out + 1 < dstSize); i++)
	{
		if (bootName[i] == '.')
		{
			continue;
		}
		dst[out++] = (bootName[i] == '_') ? '-' : bootName[i];
	}
	dst[out] = '\0';
}

internal const char *NativeAssets_RegionOfSerial(const char *bootName)
{
	if ((strncmp(bootName, "SCES", 4) == 0) || (strncmp(bootName, "SLES", 4) == 0))
	{
		return "the European (PAL) release";
	}
	if ((strncmp(bootName, "SCPS", 4) == 0) || (strncmp(bootName, "SLPS", 4) == 0) || (strncmp(bootName, "SCPM", 4) == 0) ||
	    (strncmp(bootName, "SLPM", 4) == 0))
	{
		return "the Japanese release";
	}

	return "not the USA release";
}

/* The boot executable SYSTEM.CNF names ("BOOT = cdrom:" then the file name). */
internal int NativeAssets_ParseBootName(const u8 *cnf, int cnfSize, char *dst, size_t dstSize)
{
	const NativeStr8 key = NATIVE_STR8_LIT("cdrom:");
	int i;

	for (i = 0; i + (int)key.len <= cnfSize; i++)
	{
		NativeStr8 here = {&cnf[i], key.len};
		size_t out = 0;
		int at;

		if (!NativeStr8_EqualsIgnoreCaseAscii(here, key))
		{
			continue;
		}

		at = i + (int)key.len;
		while ((at < cnfSize) && ((cnf[at] == '\\') || (cnf[at] == '/')))
		{
			at++;
		}
		while ((at < cnfSize) && (cnf[at] != ';') && (cnf[at] > ' ') && (out + 1 < dstSize))
		{
			dst[out++] = (char)NativeStr8_ToUpperAscii(cnf[at++]);
		}
		dst[out] = '\0';
		return out != 0;
	}

	return 0;
}

/* Other regions use the same file names with different contents and layout,
 * so they pass every file check and then fail somewhere mid-game. */
internal int NativeAssets_ValidateDiscRegion(void)
{
	u8 *cnf;
	int cnfSize;
	char bootName[32];
	char serial[32];
	int parsed;

	if (!NativeDiscImage_ReadFileBytes("SYSTEM.CNF", 0, &cnf, &cnfSize))
	{
		NativeAssets_SetFailure("The disc image is not a PlayStation game\ndisc: it has no SYSTEM.CNF.\n\n"
		                        "CTR 3D needs the USA (NTSC-U) release of\nCrash Team Racing, SCUS-94426.");
		return 0;
	}

	parsed = NativeAssets_ParseBootName(cnf, cnfSize, bootName, sizeof(bootName));
	free(cnf);

	if (parsed && (strcmp(bootName, NATIVE_ASSETS_RETAIL_BOOT) == 0))
	{
		return 1;
	}

	NativeAssets_FormatSerial(serial, sizeof(serial), parsed ? bootName : "unknown");
	NativeAssets_SetFailure("This is not the USA (NTSC-U) disc.\n\n"
	                        "ctr-u.bin is %s,\n"
	                        "%s.\n\n"
	                        "CTR 3D needs the USA release of Crash\n"
	                        "Team Racing, SCUS-94426. Other regions\n"
	                        "lay their data out differently.",
	                        serial, NativeAssets_RegionOfSerial(parsed ? bootName : ""));
	return 0;
}

/* Reads the first `size` bytes of the copy of BIGFILE.BIG the game will use
 * (an extracted override first, as NativeCD_OpenFile does) and its size. */
internal int NativeAssets_ReadBigfileHead(u8 *dst, u32 size, u64 *fileSizeOut)
{
	struct NativeAssetsHostRange range;
	struct NativeDiscImageFile discFile;
	int ok;

	if (NativeAssets_OpenHostRange(NATIVE_ASSETS_BIGFILE_PATH, &range))
	{
		*fileSizeOut = (u64)range.size;
		ok = ((u64)size <= *fileSizeOut) && NativeAssets_ReadHostRange(&range, 0, dst, size);
		NativeAssets_CloseHostRange(&range);
		return ok;
	}

	if (!NativeDiscImage_FindFile(NATIVE_ASSETS_BIGFILE_PATH, &discFile))
	{
		return 0;
	}

	*fileSizeOut = discFile.size;
	return NativeDiscImage_ReadDataSectors(&discFile, 0, size / LOAD_CD_DATA_SECTOR_SIZE, dst);
}

/* BIGFILE.BIG's own directory (a count, then a {sector offset, byte size}
 * pair per entry) must stay inside the file, or the level loads that follow
 * it read past the end. */
internal int NativeAssets_ValidateBigfile(void)
{
	const u32 headerBytes = LOAD_BIGFILE_HEADER_SECTORS * LOAD_CD_DATA_SECTOR_SIZE;
	const u32 maxEntries = (headerBytes - 8u) / 8u;
	u8 *header = (u8 *)malloc(headerBytes);
	u64 bigSize = 0;
	u32 numEntry;
	u32 i;

	if (header == NULL)
	{
		return 1;
	}

	if (!NativeAssets_ReadBigfileHead(header, headerBytes, &bigSize))
	{
		free(header);
		NativeAssets_SetFailure("BIGFILE.BIG could not be read.\n\nThe game data is damaged or cut short.\nCopy the .bin again.");
		return 0;
	}

	numEntry = NativeAssets_ReadLE32(&header[4]);
	if ((numEntry == 0) || (numEntry > maxEntries))
	{
		free(header);
		NativeAssets_SetFailure("BIGFILE.BIG is damaged: its directory is\nunreadable (%u entries).\n\nCopy the .bin again.", (unsigned int)numEntry);
		return 0;
	}

	for (i = 0; i < numEntry; i++)
	{
		const u64 offset = (u64)NativeAssets_ReadLE32(&header[8u + i * 8u]) * LOAD_CD_DATA_SECTOR_SIZE;
		const u64 end = offset + NativeAssets_ReadLE32(&header[12u + i * 8u]);

		if (end > bigSize)
		{
			free(header);
			NativeAssets_FailTruncated("BIGFILE.BIG", bigSize, end);
			return 0;
		}
	}

	free(header);
	return 1;
}

internal int NativeAssets_CheckDiscFileFits(const char *path, const struct NativeDiscImageFile *discFile)
{
	u64 needBytes;

	if (NativeDiscImage_FileFitsImage(discFile))
	{
		return 1;
	}

	fprintf(stderr, "[CTR 3D] asset runs past the end of the disc image: %s\n", path);
	if (s_nativeAssetsFailure[0] == '\0')
	{
		needBytes = ((u64)discFile->lba + ((discFile->size + LOAD_CD_DATA_SECTOR_SIZE - 1u) / LOAD_CD_DATA_SECTOR_SIZE)) * NATIVE_DISC_IMAGE_RAW_SECTOR_SIZE;
		NativeAssets_FailTruncated("ctr-u.bin", NativeDiscImage_GetInfo()->imageBytes, needBytes);
	}
	return 0;
}

internal int NativeAssets_CheckRequiredFile(const char *path)
{
	char assetPath[NATIVE_ASSETS_PATH_MAX];
	struct NativeDiscImageFile discFile;

	if (!NativeAssets_BuildPath(path, assetPath, sizeof(assetPath)))
	{
		fprintf(stderr, "[CTR 3D] asset path too long: %s\n", path);
		return 0;
	}

	if (NativeAssets_ResolvePath(path, assetPath, sizeof(assetPath)))
	{
		return 1;
	}

	if (NativeDiscImage_FindFile(path, &discFile))
	{
		return NativeAssets_CheckDiscFileFits(path, &discFile);
	}

	fprintf(stderr, "[CTR 3D] missing asset: %s\n", assetPath);
	return 0;
}

internal int NativeAssets_ValidateXA(void)
{
	local_persist const char *xaDirs[NATIVE_ASSETS_XA_TYPE_COUNT] = {
	    "XA/MUSIC",
	    "XA/ENG/EXTRA",
	    "XA/ENG/GAME",
	};
	struct NativeAssetsByteBuffer xnf;
	u8 required[NATIVE_ASSETS_XA_TYPE_COUNT][NATIVE_ASSETS_XA_MAX_FILE_NUMBER];
	u32 numXasTotal;
	u32 numTracksTotal;
	u32 entryOffset;
	u32 entryEnd;
	u32 missing = 0;
	u32 categoryID;

	memset(required, 0, sizeof(required));

	if (!NativeAssets_ReadBytes(NATIVE_ASSETS_XNF_PATH, NATIVE_ASSET_READ_DATA_FILE, &xnf))
	{
		return 0;
	}

	if ((xnf.size < NATIVE_ASSETS_XA_HEADER_SIZE) || (NativeAssets_ReadLE32(&xnf.data[0]) != 0x464e4958) || (NativeAssets_ReadLE32(&xnf.data[4]) != 102) ||
	    (NativeAssets_ReadLE32(&xnf.data[8]) != NATIVE_ASSETS_XA_TYPE_COUNT))
	{
		NativeAssets_FreeBytes(&xnf);
		fprintf(stderr, "[CTR 3D] invalid XA manifest: %s\n", NATIVE_ASSETS_XNF_PATH);
		NativeAssets_SetFailure("The music index (XA/ENG.XNF) is damaged.\n\nCopy the .bin again.");
		return 0;
	}

	numXasTotal = NativeAssets_ReadLE32(&xnf.data[NATIVE_ASSETS_XA_NUM_XAS_TOTAL_OFFSET]);
	numTracksTotal = NativeAssets_ReadLE32(&xnf.data[NATIVE_ASSETS_XA_NUM_TRACKS_TOTAL_OFFSET]);
	entryOffset = NATIVE_ASSETS_XA_HEADER_SIZE + numXasTotal * 4u;
	entryEnd = entryOffset + numTracksTotal * NATIVE_ASSETS_XA_ENTRY_BYTES;

	if ((entryEnd < entryOffset) || (entryEnd > (u32)xnf.size))
	{
		NativeAssets_FreeBytes(&xnf);
		fprintf(stderr, "[CTR 3D] invalid XA entry table: %s\n", NATIVE_ASSETS_XNF_PATH);
		return 0;
	}

	for (categoryID = 0; categoryID < NATIVE_ASSETS_XA_TYPE_COUNT; categoryID++)
	{
		u32 numSongs = NativeAssets_ReadLE32(&xnf.data[NATIVE_ASSETS_XA_NUM_SONGS_OFFSET + categoryID * 4u]);
		u32 firstSongIndex = NativeAssets_ReadLE32(&xnf.data[NATIVE_ASSETS_XA_FIRST_SONG_INDEX_OFFSET + categoryID * 4u]);
		u32 xaID;

		if ((firstSongIndex > numTracksTotal) || (numSongs > numTracksTotal) || (firstSongIndex + numSongs > numTracksTotal))
		{
			NativeAssets_FreeBytes(&xnf);
			fprintf(stderr, "[CTR 3D] invalid XA song range in %s\n", NATIVE_ASSETS_XNF_PATH);
			return 0;
		}

		for (xaID = 0; xaID < numSongs; xaID++)
		{
			const u8 *entry = &xnf.data[entryOffset + (firstSongIndex + xaID) * NATIVE_ASSETS_XA_ENTRY_BYTES];
			required[categoryID][entry[1]] = 1;
		}
	}

	NativeAssets_FreeBytes(&xnf);

	for (categoryID = 0; categoryID < NATIVE_ASSETS_XA_TYPE_COUNT; categoryID++)
	{
		u32 fileNumber;

		for (fileNumber = 0; fileNumber < NATIVE_ASSETS_XA_MAX_FILE_NUMBER; fileNumber++)
		{
			char relativePath[256];
			char path[NATIVE_ASSETS_PATH_MAX];
			struct NativeDiscImageFile discFile;
			int written;

			if (!required[categoryID][fileNumber])
			{
				continue;
			}

			written = snprintf(relativePath, sizeof(relativePath), "%s/S%02u.XA", xaDirs[categoryID], (unsigned int)fileNumber);
			if ((written <= 0) || ((size_t)written >= sizeof(relativePath)) || !NativeAssets_BuildPath(relativePath, path, sizeof(path)))
			{
				fprintf(stderr, "[CTR 3D] XA asset path too long: %s/S%02u.XA\n", xaDirs[categoryID], (unsigned int)fileNumber);
				missing++;
				continue;
			}

			if (NativeAssets_ResolvePath(relativePath, path, sizeof(path)))
			{
				continue;
			}

			if (!NativeDiscImage_FindFile(relativePath, &discFile))
			{
				fprintf(stderr, "[CTR 3D] missing XA asset: %s\n", path);
				missing++;
			}
			else if (!NativeAssets_CheckDiscFileFits(relativePath, &discFile))
			{
				missing++;
			}
		}
	}

	return missing == 0;
}

int NativeAssets_Validate(void)
{
	const struct NativeDiscImageInfo *disc = NativeDiscImage_GetInfo();
	int ok = 1;

	s_nativeAssetsFailure[0] = '\0';

	/* An image that is present but unusable is reported as itself, even if
	 * extracted files could stand in for it: the player supplied that file,
	 * and "not found" would send them looking for the wrong problem. */
	if ((disc->status != NATIVE_DISC_IMAGE_STATUS_NOT_FOUND) && (disc->status != NATIVE_DISC_IMAGE_STATUS_OK))
	{
		NativeAssets_FailDiscImage(disc);
		fprintf(stderr, "[CTR 3D] Startup failed:\n%s\n", NativeAssets_GetFailureMessage());
		return 0;
	}

	if ((disc->status == NATIVE_DISC_IMAGE_STATUS_OK) && !NativeAssets_ValidateDiscRegion())
	{
		fprintf(stderr, "[CTR 3D] Startup failed:\n%s\n", NativeAssets_GetFailureMessage());
		return 0;
	}

	ok &= NativeAssets_CheckRequiredFile(NATIVE_ASSETS_BIGFILE_PATH);
	ok &= NativeAssets_CheckRequiredFile(NATIVE_ASSETS_KART_HWL_PATH);
	ok &= NativeAssets_CheckRequiredFile(NATIVE_ASSETS_TEST_STR_PATH);
	ok &= NativeAssets_CheckRequiredFile(NATIVE_ASSETS_XNF_PATH);

	if (ok)
	{
		ok &= NativeAssets_ValidateXA();
	}

	if (ok)
	{
		ok &= NativeAssets_ValidateBigfile();
	}

	if (!ok)
	{
		if (s_nativeAssetsFailure[0] == '\0')
		{
			if (disc->status == NATIVE_DISC_IMAGE_STATUS_NOT_FOUND)
			{
				NativeAssets_FailNotFound();
			}
			else
			{
				NativeAssets_SetFailure("The disc image is missing game files.\n\n"
				                        "It is not an unmodified USA (NTSC-U)\n"
				                        "Crash Team Racing dump. Dump the disc\n"
				                        "again as a raw MODE2/2352 .bin.");
			}
		}

		NativeAssets_PrintHeader();
		NativeAssets_PrintFooter();
		fprintf(stderr, "[CTR 3D] Startup failed:\n%s\n", NativeAssets_GetFailureMessage());
	}

	return ok;
}
