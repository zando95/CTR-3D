/*
 * Minimal SDL3 compatibility surface for the Nintendo 3DS build.
 *
 * The 3DS target does not link SDL. The platform layer (native_platform.c,
 * native_input.c, native_audio.c, native_perf.c) still names SDL types and
 * entry points, and those files are shared with the host build -- so rather
 * than fork them, this header supplies just enough of the API for them to
 * compile unchanged, and sdl3_compat_3ds.c backs it with libctru.
 *
 * Two different fidelity levels live behind this header:
 *
 *   REAL     -- timing (SDL_GetPerformanceCounter/Frequency, SDL_Delay) and
 *               keyboard state. VSync() in native_platform.c is the game's
 *               30fps pacing engine and reads the tick clock directly, so it
 *               must be genuine or the frame gate is meaningless. Keyboard
 *               state is driven from 3DS HID, which is how buttons reach the
 *               game: native_input.c already maps scancodes to PSX pad bits.
 *
 *   INERT    -- windowing, gamepad hotplug, audio device. These report "not
 *               present" so the shared code takes its own no-device paths.
 *               Audio still MIXES (the SPU emulation is SDL-free); PCM pushed
 *               into this shim's stream goes nowhere -- real output is the
 *               separate ndsp path in n3ds_audio_thread.c.
 *
 * Values here do not have to match real SDL's numbering -- nothing links
 * against SDL on this target -- they only have to be self-consistent. The
 * scancode and gamepad values do mirror SDL 3.4.10 anyway, to keep the shared
 * files readable against upstream.
 */

#ifndef SDL3_COMPAT_3DS_H
#define SDL3_COMPAT_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Calling-convention marker on SDL callbacks. Plain C everywhere on ARM. */
#define SDLCALL

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;

/* ---------------------------------------------------------------- init --- */

typedef Uint32 SDL_InitFlags;

#define SDL_INIT_AUDIO   0x00000010u
#define SDL_INIT_VIDEO   0x00000020u
#define SDL_INIT_HAPTIC  0x00001000u
#define SDL_INIT_GAMEPAD 0x00002000u

bool SDL_Init(SDL_InitFlags flags);
void SDL_Quit(void);
bool SDL_InitSubSystem(SDL_InitFlags flags);
void SDL_QuitSubSystem(SDL_InitFlags flags);

const char *SDL_GetError(void);
void SDL_free(void *mem);

/* Asset root. The startup working directory differs between the Homebrew
 * Launcher, 3dslink and emulators, so the implementation probes plausible
 * roots rather than trusting cwd (see sdl3_compat_3ds.c). */
const char *SDL_GetBasePath(void);

/* True when `name` is readable directly under one of those same candidate
 * roots. For opt-in marker files dropped next to the .3dsx; presence is the
 * whole signal, contents are never read. */
bool N3DS_AppDirHasFile(const char *name);

/* --------------------------------------------------------------- timer --- */

Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
void SDL_Delay(Uint32 ms);

/* --------------------------------------------------------------- video --- */

typedef struct SDL_Window SDL_Window;
typedef Uint64 SDL_WindowFlags;

#define SDL_WINDOW_FULLSCREEN 0x0000000000000001ull

void SDL_DestroyWindow(SDL_Window *window);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window);
bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen);
bool SDL_GL_SwapWindow(SDL_Window *window);
bool SDL_ShowCursor(void);
bool SDL_HideCursor(void);

/* -------------------------------------------------------------- events --- */

typedef enum SDL_EventType
{
	SDL_EVENT_QUIT = 0x100,

	SDL_EVENT_WINDOW_RESIZED = 0x200,
	SDL_EVENT_WINDOW_CLOSE_REQUESTED,
	SDL_EVENT_WINDOW_ENTER_FULLSCREEN,
	SDL_EVENT_WINDOW_LEAVE_FULLSCREEN,

	SDL_EVENT_KEY_DOWN = 0x300,
	SDL_EVENT_KEY_UP,

	SDL_EVENT_GAMEPAD_ADDED = 0x650,
	SDL_EVENT_GAMEPAD_REMOVED
} SDL_EventType;

typedef Uint32 SDL_JoystickID;

typedef enum SDL_Scancode
{
	SDL_SCANCODE_UNKNOWN = 0,

	SDL_SCANCODE_A = 4,
	SDL_SCANCODE_C = 6,
	SDL_SCANCODE_F = 9,
	SDL_SCANCODE_V = 25,
	SDL_SCANCODE_X = 27,
	SDL_SCANCODE_Z = 29,
	SDL_SCANCODE_1 = 30,

	SDL_SCANCODE_RETURN = 40,
	SDL_SCANCODE_SPACE = 44,
	SDL_SCANCODE_LEFTBRACKET = 47,
	SDL_SCANCODE_RIGHTBRACKET = 48,

	SDL_SCANCODE_F1 = 58,
	SDL_SCANCODE_F2 = 59,
	SDL_SCANCODE_F3 = 60,
	SDL_SCANCODE_F4 = 61,
	SDL_SCANCODE_F5 = 62,
	SDL_SCANCODE_F6 = 63,
	SDL_SCANCODE_F7 = 64,
	SDL_SCANCODE_F8 = 65,
	SDL_SCANCODE_F9 = 66,
	SDL_SCANCODE_F10 = 67,
	SDL_SCANCODE_F11 = 68,
	SDL_SCANCODE_F12 = 69,

	// F13 (value matches real SDL3's SDL_scancode.h) -- the 3DS has no
	// physical keyboard, so this scancode is never actually produced by any
	// input path here; it exists only so shared unity-TU code (native_platform.c's
	// Platform_HandleKey switch, e.g. the native track renderer's PC debug
	// key) can reference SDL_SCANCODE_F13 without a platform #ifdef and
	// still compile on 3DS, even though the case can never fire there.
	SDL_SCANCODE_F13 = 104,

	SDL_SCANCODE_RIGHT = 79,
	SDL_SCANCODE_LEFT = 80,
	SDL_SCANCODE_DOWN = 81,
	SDL_SCANCODE_UP = 82,

	SDL_SCANCODE_LCTRL = 224,
	SDL_SCANCODE_LSHIFT = 225,
	SDL_SCANCODE_LALT = 226,
	SDL_SCANCODE_RCTRL = 228,
	SDL_SCANCODE_RSHIFT = 229,
	SDL_SCANCODE_RALT = 230,

	SDL_SCANCODE_COUNT = 512
} SDL_Scancode;

typedef struct SDL_KeyboardEvent
{
	Uint32 type;
	SDL_Scancode scancode;
	bool repeat;
} SDL_KeyboardEvent;

typedef struct SDL_WindowEvent
{
	Uint32 type;
	Sint32 data1;
	Sint32 data2;
} SDL_WindowEvent;

typedef struct SDL_GamepadDeviceEvent
{
	Uint32 type;
	SDL_JoystickID which;
} SDL_GamepadDeviceEvent;

typedef union SDL_Event
{
	Uint32 type;
	SDL_KeyboardEvent key;
	SDL_WindowEvent window;
	SDL_GamepadDeviceEvent gdevice;
	Uint8 padding[64];
} SDL_Event;

bool SDL_PollEvent(SDL_Event *event);
void SDL_PumpEvents(void);

/* Asks for the same exit a HOME-menu Close takes: the next SDL_PollEvent reports SDL_EVENT_QUIT, so the
 * one shutdown path in Platform_PollHostEvents runs. The request is never cleared. */
void N3DS_RequestQuit(void);

/* ------------------------------------------------------------ keyboard --- */

const bool *SDL_GetKeyboardState(int *numkeys);

/* ------------------------------------------------------------- gamepad --- */

typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;

typedef enum SDL_GamepadButton
{
	SDL_GAMEPAD_BUTTON_INVALID = -1,
	SDL_GAMEPAD_BUTTON_SOUTH,
	SDL_GAMEPAD_BUTTON_EAST,
	SDL_GAMEPAD_BUTTON_WEST,
	SDL_GAMEPAD_BUTTON_NORTH,
	SDL_GAMEPAD_BUTTON_BACK,
	SDL_GAMEPAD_BUTTON_GUIDE,
	SDL_GAMEPAD_BUTTON_START,
	SDL_GAMEPAD_BUTTON_LEFT_STICK,
	SDL_GAMEPAD_BUTTON_RIGHT_STICK,
	SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
	SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
	SDL_GAMEPAD_BUTTON_DPAD_UP,
	SDL_GAMEPAD_BUTTON_DPAD_DOWN,
	SDL_GAMEPAD_BUTTON_DPAD_LEFT,
	SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
	SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;

typedef enum SDL_GamepadAxis
{
	SDL_GAMEPAD_AXIS_INVALID = -1,
	SDL_GAMEPAD_AXIS_LEFTX,
	SDL_GAMEPAD_AXIS_LEFTY,
	SDL_GAMEPAD_AXIS_RIGHTX,
	SDL_GAMEPAD_AXIS_RIGHTY,
	SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
	SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
	SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id);
void SDL_CloseGamepad(SDL_Gamepad *gamepad);
bool SDL_IsGamepad(SDL_JoystickID instance_id);
bool SDL_GamepadConnected(SDL_Gamepad *gamepad);
SDL_JoystickID *SDL_GetGamepads(int *count);
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis);
SDL_Joystick *SDL_GetGamepadJoystick(SDL_Gamepad *gamepad);
int SDL_AddGamepadMappingsFromFile(const char *file);
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 durationMs);
SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick);

/* --------------------------------------------------------------- audio --- */

typedef Uint32 SDL_AudioDeviceID;
typedef struct SDL_AudioStream SDL_AudioStream;

typedef enum SDL_AudioFormat
{
	SDL_AUDIO_UNKNOWN = 0x0000,
	SDL_AUDIO_U8 = 0x0008,
	SDL_AUDIO_S8 = 0x8008,
	SDL_AUDIO_S16 = 0x8010,
	SDL_AUDIO_S32 = 0x8020,
	SDL_AUDIO_F32 = 0x8120
} SDL_AudioFormat;

typedef struct SDL_AudioSpec
{
	SDL_AudioFormat format;
	int channels;
	int freq;
} SDL_AudioSpec;

typedef void (*SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount);

#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);
bool SDL_ClearAudioStream(SDL_AudioStream *stream);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sampleFrames);
bool SDL_GetAudioStreamFormat(SDL_AudioStream *stream, SDL_AudioSpec *srcSpec, SDL_AudioSpec *dstSpec);
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream);
bool SDL_LockAudioStream(SDL_AudioStream *stream);
bool SDL_UnlockAudioStream(SDL_AudioStream *stream);
const char *SDL_GetCurrentAudioDriver(void);

/* --------------------------------------------------------------- hints --- */

#define SDL_HINT_AUDIO_DRIVER               "SDL_AUDIO_DRIVER"
#define SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "SDL_AUDIO_DEVICE_SAMPLE_FRAMES"
#define SDL_HINT_WINDOW_ALLOW_TOPMOST       "SDL_WINDOW_ALLOW_TOPMOST"

bool SDL_SetHint(const char *name, const char *value);
const char *SDL_GetHint(const char *name);

#endif
