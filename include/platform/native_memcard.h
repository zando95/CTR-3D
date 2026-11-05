#ifndef PLATFORM_NATIVE_MEMCARD_H
#define PLATFORM_NATIVE_MEMCARD_H

enum NativeMemcardResult
{
	NATIVE_MEMCARD_OK = 0,
	NATIVE_MEMCARD_NOT_FOUND,
	NATIVE_MEMCARD_OPEN_FAILED,
	NATIVE_MEMCARD_IO_ERROR,
};

enum NativeMemcardResult NativeMemcard_SetRoot(const char *root_path);
void NativeMemcard_ClearRoot(void);
enum NativeMemcardResult NativeMemcard_CloneRoot(const char *src_root, const char *dst_root);
enum NativeMemcardResult NativeMemcard_CloneCurrentRoot(const char *dst_root);
enum NativeMemcardResult NativeMemcard_RemoveRoot(const char *root_path);
int NativeMemcard_FileExists(const char *save_name);
int NativeMemcard_FindFirstFile(const char *pattern, char *dst_name, int dst_size);
int NativeMemcard_FindNextFile(char *dst_name, int dst_size);
enum NativeMemcardResult NativeMemcard_RemoveFile(const char *save_name);
enum NativeMemcardResult NativeMemcard_ReadSaveData(const char *save_name, unsigned char *dst, int byte_count, int data_offset);
// The copy the last successful write replaced; the fallback when the primary is
// missing or fails its checksum.
enum NativeMemcardResult NativeMemcard_ReadBackupSaveData(const char *save_name, unsigned char *dst, int byte_count, int data_offset);

// Runs on the game thread once a queued write has landed. elapsed_ms is the
// time the write itself took (0 where it is not measured).
typedef void (*NativeMemcardWriteDone)(enum NativeMemcardResult result, unsigned int elapsed_ms, void *user);

// Saves icon then src under save_name. The bytes are copied before this
// returns. Writes land in the order they were queued, and every read, erase
// or root change first waits for the queue to empty, so nothing ever sees a
// save older than one already queued. On the 3DS the write runs on the save
// writer and done (may be NULL) runs from a later QueueWrite, PumpWrites or
// FinishWrites; elsewhere, or when the writer is not running, the write runs
// inline and done runs before this returns.
void NativeMemcard_QueueWrite(const char *save_name, const void *icon, int icon_byte_count, const unsigned char *src, int byte_count,
                              NativeMemcardWriteDone done, void *user);
// Overwrites the file at path (a full host path outside the memory card) with
// src, in the same queue and order as the saves. No backup copy is kept: for
// small settings files that fall back to their defaults when missing.
void NativeMemcard_QueueFileWrite(const char *path, const unsigned char *src, int byte_count);
// Runs the done callbacks of writes that have landed. Never blocks.
void NativeMemcard_PumpWrites(void);
// Blocks until every queued write has landed, then runs their callbacks.
void NativeMemcard_FinishWrites(void);
// The save writer's lifetime. Stop finishes every queued write first. Both
// are no-ops where there is no writer.
void NativeMemcard_StartWriter(void);
void NativeMemcard_StopWriter(void);

#endif
