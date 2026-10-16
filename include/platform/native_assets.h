#ifndef NATIVE_ASSETS_H
#define NATIVE_ASSETS_H

#include <stddef.h>
#include <stdio.h>

#include "platform/native_str8.h"

enum NativeAssetReadMode
{
	NATIVE_ASSET_READ_DATA_FILE,
	NATIVE_ASSET_READ_RAW_CD_SECTORS,
};

struct NativeAssetsByteBuffer
{
	u8 *data;
	int size;
};

// NOTE(aalhendi): Open-once, seek+read-many-times handle for callers that need
// bounded ranges out of a host file (e.g. one XA sector at a time) instead of
// NativeAssets_ReadBytes's whole-file malloc.
struct NativeAssetsHostRange
{
	FILE *file;
	long size;
};

int NativeAssets_Init(const char *executableBasePath);
const char *NativeAssets_GetBaseDir(void);
const char *NativeAssets_GetAssetDir(void);
int NativeAssets_BuildPathStr8(NativeStr8 relativePath, char *dst, size_t dstSize);
int NativeAssets_BuildPath(const char *relativePath, char *dst, size_t dstSize);
int NativeAssets_ResolvePathStr8(NativeStr8 relativePath, char *dst, size_t dstSize);
int NativeAssets_ResolvePath(const char *relativePath, char *dst, size_t dstSize);
FILE *NativeAssets_OpenHostStr8(NativeStr8 relativePath, const char *mode);
FILE *NativeAssets_OpenHost(const char *relativePath, const char *mode);
FILE *NativeAssets_OpenHostBigfile(const char *mode);
int NativeAssets_ReadBytes(const char *path, int readMode, struct NativeAssetsByteBuffer *bytes);
void NativeAssets_FreeBytes(struct NativeAssetsByteBuffer *bytes);
int NativeAssets_OpenHostRange(const char *relativePath, struct NativeAssetsHostRange *range);
int NativeAssets_ReadHostRange(struct NativeAssetsHostRange *range, long offset, void *dst, size_t size);
void NativeAssets_CloseHostRange(struct NativeAssetsHostRange *range);
int NativeAssets_Validate(void);
/* Why Init or Validate refused the assets, in words for the player: multi-line,
 * at most 44 columns per line, no trailing newline. */
const char *NativeAssets_GetFailureMessage(void);

#endif
