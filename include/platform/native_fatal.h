#ifndef NATIVE_FATAL_H
#define NATIVE_FATAL_H

/*
 * Player-facing fatal errors. The message is plain text in words a player can
 * act on: lines separated by '\n', at most 44 columns each so the 3DS console
 * (50 columns, indented 2) never wraps mid-word, no trailing newline.
 */

/* Shows the message on the platform's error screen (the 3DS top-screen
 * console, a message box on PC) and returns once it is dismissed. Usable
 * before Platform_Init and after a failed one. */
void NativePlatform_ShowFatal(const char *message);

/* For failures once the game is running: logs the message, shuts the platform
 * down, shows the message and exits. Never returns. */
void NativePlatform_FatalExit(const char *message);

/* Why the last Platform_Init failed, as a player-facing message. */
const char *NativePlatform_InitFailure(void);

#endif
