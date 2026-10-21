/*
 * libctru backing for the SDL3 compatibility surface. See sdl3_compat_3ds.h
 * for why this exists and which parts are real versus inert.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "n3ds_audio_thread.h"
#include "n3ds_backend.h"
#include "sdl3_compat_3ds.h"

/* ================================================================ init === */

bool SDL_Init(SDL_InitFlags flags)
{
	(void)flags;
	return true;
}

void SDL_Quit(void)
{
}

bool SDL_InitSubSystem(SDL_InitFlags flags)
{
	(void)flags;
	return true;
}

void SDL_QuitSubSystem(SDL_InitFlags flags)
{
	(void)flags;
}

const char *SDL_GetError(void)
{
	return "no SDL on 3DS";
}

void SDL_free(void *mem)
{
	(void)mem;
}

/*
 * The working directory a .3dsx starts in is not dependable: it differs
 * between the Homebrew Launcher, 3dslink and an emulator. Rather than guess,
 * probe the plausible roots and use whichever one actually holds the install.
 * Order matters only in that the working directory is tried first, so a
 * self-contained install wins. Every consumer that looks for a file "next to
 * the .3dsx" must walk this same list, or two of them will disagree about
 * where the app lives.
 */
static const char *const s_n3dsAppDirCandidates[] = {
	".",
	"sdmc:/3ds/ctr_native",
	"sdmc:/3ds/ctr-native",
	"sdmc:/ctr-native",
	"sdmc:/",
};

#define N3DS_APP_DIR_CANDIDATE_COUNT (sizeof(s_n3dsAppDirCandidates) / sizeof(s_n3dsAppDirCandidates[0]))

bool N3DS_AppDirHasFile(const char *name)
{
	unsigned int i;

	if (name == NULL)
	{
		return false;
	}

	for (i = 0; i < N3DS_APP_DIR_CANDIDATE_COUNT; i++)
	{
		char probe[128];
		FILE *f;

		snprintf(probe, sizeof(probe), "%s/%s", s_n3dsAppDirCandidates[i], name);

		f = fopen(probe, "rb");
		if (f != NULL)
		{
			fclose(f);
			return true;
		}
	}

	return false;
}

const char *SDL_GetBasePath(void)
{
	static char s_resolved[64];
	unsigned int i;
	int anyDirReadable = 0;

	for (i = 0; i < N3DS_APP_DIR_CANDIDATE_COUNT; i++)
	{
		const char *const candidate = s_n3dsAppDirCandidates[i];
		char probe[96];
		char note[160];
		struct stat st;
		int hasDir;
		int hasAssets;
		FILE *f;

		hasDir = (stat(candidate, &st) == 0) && S_ISDIR(st.st_mode);

		snprintf(probe, sizeof(probe), "%s/assets", candidate);
		hasAssets = (stat(probe, &st) == 0) && S_ISDIR(st.st_mode);

		anyDirReadable |= hasDir;

		snprintf(probe, sizeof(probe), "%s/assets/ctr-u.bin", candidate);
		f = fopen(probe, "rb");

		/* Report each candidate separately: "dir but no assets" means the wrong
		 * folder, "assets but no bin" means the image was not copied, and no
		 * readable directory at all means the SD mount itself is the problem. */
		snprintf(note, sizeof(note), "[CTR-3DS] probe %-24s dir=%s assets=%s bin=%s",
		         candidate, hasDir ? "yes" : "no", hasAssets ? "yes" : "no",
		         (f != NULL) ? "YES" : "no");
		N3DS_DebugLog(note);

		if (f != NULL)
		{
			fclose(f);

			snprintf(s_resolved, sizeof(s_resolved), "%s", candidate);
			N3DS_DebugLog("[CTR-3DS] using disc image under the root above");
			return s_resolved;
		}
	}

	if (anyDirReadable)
	{
		N3DS_DebugLog("[CTR-3DS] SD is readable but no assets/ctr-u.bin exists -- copy the "
		              "disc image to <SD>/3ds/ctr_native/assets/ctr-u.bin");
	}
	else
	{
		N3DS_DebugLog("[CTR-3DS] no candidate directory was even readable -- SD mount problem");
	}

	return NULL;
}

/* =============================================================== timer === */
/*
 * Real, and load-bearing. VSync() derives the 60Hz vblank target from this
 * clock, and the game's 30fps gate counts those vblanks. svcGetSystemTick runs
 * at the ARM11 base clock regardless of New3DS speed mode, so the pacing stays
 * correct whether or not the CPU is clocked up.
 */

Uint64 SDL_GetPerformanceCounter(void)
{
	return (Uint64)N3DS_GetTickCount();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
	return (Uint64)N3DS_GetTickFrequency();
}

void SDL_Delay(Uint32 ms)
{
	N3DS_SleepMs(ms);
}

/* =============================================================== video === */
/* No host window exists; g_window stays NULL and these keep that path quiet. */

void SDL_DestroyWindow(SDL_Window *window)
{
	(void)window;
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	(void)window;
	return 0;
}

bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
	(void)window;
	if (w != NULL)
	{
		*w = 400;
	}
	if (h != NULL)
	{
		*h = 240;
	}
	return true;
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	(void)window;
	(void)fullscreen;
	return true;
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
	(void)window;
	return true;
}

bool SDL_ShowCursor(void)
{
	return true;
}

bool SDL_HideCursor(void)
{
	return true;
}

/* ============================================================ keyboard === */
/*
 * 3DS HID is surfaced as keyboard state rather than as a gamepad, because
 * native_input.c's default keyboard mapping already turns these scancodes into
 * PSX pad bits (see NativeInput_DefaultMappings). Routing through the existing
 * mapping avoids duplicating the pad-bus logic.
 *
 * Face buttons map by PHYSICAL POSITION, not by letter. The 3DS diamond sits
 * where the PlayStation diamond does, so bottom->Cross, left->Square,
 * top->Triangle, right->Circle:
 *
 *        X                      /\                 X -> Triangle
 *      Y   A                  []    O              Y -> Square
 *        B                      ><                 A -> Circle
 *                                                  B -> Cross
 *
 * Matching them by letter instead (3DS A -> Cross because both are "A"-ish)
 * puts accelerate on the right-hand button rather than the bottom one, which
 * is wrong under the thumb for anyone who has played the original.
 */

static bool s_keyState[SDL_SCANCODE_COUNT];

static void N3DS_SetKey(SDL_Scancode code, bool down)
{
	s_keyState[code] = down;
}

static void N3DS_PumpInput(void)
{
	const unsigned int held = N3DS_ReadButtons();

	N3DS_SetKey(SDL_SCANCODE_C, (held & N3DS_BTN_B) != 0); /* B (bottom) -> cross    : accelerate */
	N3DS_SetKey(SDL_SCANCODE_X, (held & N3DS_BTN_Y) != 0); /* Y (left)   -> square   : brake     */
	N3DS_SetKey(SDL_SCANCODE_V, (held & N3DS_BTN_A) != 0); /* A (right)  -> circle   : fire      */
	N3DS_SetKey(SDL_SCANCODE_Z, (held & N3DS_BTN_X) != 0); /* X (top)    -> triangle : look back */

	N3DS_SetKey(SDL_SCANCODE_LSHIFT, (held & N3DS_BTN_L) != 0); /* L1 */
	N3DS_SetKey(SDL_SCANCODE_RSHIFT, (held & N3DS_BTN_R) != 0); /* R1 */
	N3DS_SetKey(SDL_SCANCODE_LCTRL, (held & N3DS_BTN_ZL) != 0); /* L2, see native_input.c kc_l2 */
	N3DS_SetKey(SDL_SCANCODE_RCTRL, (held & N3DS_BTN_ZR) != 0); /* R2, see native_input.c kc_r2 */

	N3DS_SetKey(SDL_SCANCODE_UP, (held & N3DS_BTN_UP) != 0);
	N3DS_SetKey(SDL_SCANCODE_DOWN, (held & N3DS_BTN_DOWN) != 0);
	N3DS_SetKey(SDL_SCANCODE_LEFT, (held & N3DS_BTN_LEFT) != 0);
	N3DS_SetKey(SDL_SCANCODE_RIGHT, (held & N3DS_BTN_RIGHT) != 0);

	N3DS_SetKey(SDL_SCANCODE_RETURN, (held & N3DS_BTN_START) != 0);
	N3DS_SetKey(SDL_SCANCODE_SPACE, (held & N3DS_BTN_SELECT) != 0);
}

const bool *SDL_GetKeyboardState(int *numkeys)
{
	if (numkeys != NULL)
	{
		*numkeys = SDL_SCANCODE_COUNT;
	}
	return s_keyState;
}

/* ============================================================== events === */

static int s_sdlN3dsQuitRequested;

void N3DS_RequestQuit(void)
{
	s_sdlN3dsQuitRequested = 1;
}

bool SDL_PollEvent(SDL_Event *event)
{
	int mainLoopActive;

	N3DS_PumpInput();

	/*
	 * HOME/power close the applet, and the main menu's QUIT row asks for the same. Report either as a
	 * quit event so the existing handler in Platform_PollHostEvents performs the normal shutdown,
	 * rather than adding a second exit path. N3DS_MainLoopActive runs every poll (it ticks the
	 * worker-thread heartbeat monitor), so the request flag never short-circuits it.
	 */
	mainLoopActive = N3DS_MainLoopActive();
	if ((mainLoopActive == 0) || (s_sdlN3dsQuitRequested != 0))
	{
		if (event != NULL)
		{
			memset(event, 0, sizeof(*event));
			event->type = SDL_EVENT_QUIT;
		}
		return true;
	}

	return false;
}

void SDL_PumpEvents(void)
{
	N3DS_PumpInput();
}

/* ============================================================= gamepad === */
/*
 * Reported as "no gamepads present". The built-in pad arrives via the keyboard
 * path above, so exposing it here as well would double-drive the pad bus.
 */

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id)
{
	(void)instance_id;
	return NULL;
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
	(void)gamepad;
}

bool SDL_IsGamepad(SDL_JoystickID instance_id)
{
	(void)instance_id;
	return false;
}

bool SDL_GamepadConnected(SDL_Gamepad *gamepad)
{
	(void)gamepad;
	return false;
}

SDL_JoystickID *SDL_GetGamepads(int *count)
{
	if (count != NULL)
	{
		*count = 0;
	}
	return NULL;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
	(void)gamepad;
	(void)button;
	return false;
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
	(void)gamepad;
	(void)axis;
	return 0;
}

SDL_Joystick *SDL_GetGamepadJoystick(SDL_Gamepad *gamepad)
{
	(void)gamepad;
	return NULL;
}

int SDL_AddGamepadMappingsFromFile(const char *file)
{
	(void)file;
	return 0;
}

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 durationMs)
{
	(void)gamepad;
	(void)low;
	(void)high;
	(void)durationMs;
	return false;
}

SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick)
{
	(void)joystick;
	return 0;
}

/* =============================================================== audio === */
/*
 * A null sink that satisfies the fixed PCM contract native_audio.c asserts on
 * (S16 stereo at the native rate). Opening "succeeds" so the SPU emulation
 * stays fully live -- voices, ADSR, reverb and XA all still mix each vblank --
 * and only the final handoff to hardware is dropped. Real 3DS audio output is
 * the separate ndsp path in n3ds_audio_thread.c, which bypasses this shim
 * entirely.
 */

struct SDL_AudioStream
{
	SDL_AudioSpec spec;
	int open;
};

static struct SDL_AudioStream s_nullStream;

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata)
{
	(void)devid;
	(void)callback;
	(void)userdata;

	memset(&s_nullStream, 0, sizeof(s_nullStream));
	if (spec != NULL)
	{
		s_nullStream.spec = *spec;
	}
	s_nullStream.open = 1;

	return &s_nullStream;
}

void SDL_DestroyAudioStream(SDL_AudioStream *stream)
{
	if (stream != NULL)
	{
		stream->open = 0;
	}
}

bool SDL_ClearAudioStream(SDL_AudioStream *stream)
{
	(void)stream;
	return true;
}

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream)
{
	(void)stream;
	return 1;
}

bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sampleFrames)
{
	(void)devid;
	if (spec != NULL)
	{
		*spec = s_nullStream.spec;
	}
	if (sampleFrames != NULL)
	{
		*sampleFrames = 0;
	}
	return true;
}

bool SDL_GetAudioStreamFormat(SDL_AudioStream *stream, SDL_AudioSpec *srcSpec, SDL_AudioSpec *dstSpec)
{
	if (stream == NULL)
	{
		return false;
	}

	/* Grant exactly what was requested; the caller hard-fails on any mismatch. */
	if (srcSpec != NULL)
	{
		*srcSpec = stream->spec;
	}
	if (dstSpec != NULL)
	{
		*dstSpec = stream->spec;
	}
	return true;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream *stream)
{
	(void)stream;
	return 0;
}

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len)
{
	(void)stream;
	(void)buf;
	(void)len;
	return true;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream)
{
	(void)stream;
	return true;
}

bool SDL_LockAudioStream(SDL_AudioStream *stream)
{
	(void)stream;
	N3DS_AudioLock();
	return true;
}

bool SDL_UnlockAudioStream(SDL_AudioStream *stream)
{
	(void)stream;
	N3DS_AudioUnlock();
	return true;
}

const char *SDL_GetCurrentAudioDriver(void)
{
	return "n3ds-null";
}

/* =============================================================== hints === */

bool SDL_SetHint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return true;
}

const char *SDL_GetHint(const char *name)
{
	(void)name;
	return NULL;
}
