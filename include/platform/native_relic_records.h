#ifndef PLATFORM_NATIVE_RELIC_RECORDS_H
#define PLATFORM_NATIVE_RELIC_RECORDS_H

/* RELIC RACES records (platform/native_relic_records.c, mode described in native_arcade_modes.h).
 * Only the shutdown path needs this much of it. */

/* Writes the records file if a run changed it since the last write. Call it wherever a finished
 * race is left and at platform shutdown. No-op when nothing is pending, and on PC. */
void NativeRelic_Flush(void);

#endif
