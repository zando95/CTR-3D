#ifndef NATIVE_MIRROR_H
#define NATIVE_MIRROR_H

/* Mirror mode (3DS only).
 *
 * The 3D world is drawn mirrored left to right while the race underneath is unchanged: track, physics,
 * AI, ghosts, times and records are all retail's. The player's steering is swapped to match and world
 * sounds swap sides. The racers, the podium characters, the bosses' cutscene heads and Aku Aku / Uka
 * Uka stay unmirrored; the HUD, text and menus are never mirrored, and the minimap is drawn as the
 * mirror image of the track. Behind the menus, the checkered flag of the menu background and loading
 * screens is mirrored while the mode is on.
 *
 * Winning all four Arcade cups on ULTRA HARD unlocks it (NativeXDiff_UltraHardComplete); a MIRROR row
 * above OPTIONS on the main menu then switches it. The switch is not saved: every launch starts with it
 * off.
 *
 * On PC nothing turns it on. Defined in platform/native_mirror.c. */

#ifdef CTR_NATIVE

/* The main-menu switch. Only the MIRROR row writes it. */
extern int g_mirrorMode;

/* Mirror mode is unlocked on the loaded card. */
int NativeMirror_Unlocked(void);

/* Switched on and unlocked: the menu flag is mirrored. */
int NativeMirror_Active(void);

/* The world is mirrored now: active and not on the main menu, whose 3D characters stay as they are.
 * Every hook below tests this one predicate, so the picture, the steering, the sound and the minimap
 * always agree. */
int NativeMirror_World(void);

/* VehPhysProc_Driving_PhysLinear: a driver's stick X (centre 0x80), swapped left/right in a mirrored
 * world. The attract demo replays recorded input, which already steers the race underneath, so it is
 * left alone. */
int NativeMirror_SteerStick(int stickLX);

/* A world sound's pan byte (0x00 left .. 0xFF right), swapped in a mirrored world. Applied where a 3D
 * sound's pan is computed (PlaySound3D, the AI and player engines, skids), never to music or menu
 * sounds, so the stored pan stays consistent through later volume updates. */
u32 NativeMirror_SfxLR(u32 lr);

/* RenderBucket_QueueDraw: whether an instance stays unmirrored in a mirrored world. Its model-space X is
 * then negated so the screen mirror cancels, and a racer in its drive animation also draws the steering
 * frame from the other side, or it would lean out of its turns. Render-time only: no game state changes. */
enum NativeMirrorKeep
{
	NATIVE_MIRROR_KEEP_NONE = 0,
	NATIVE_MIRROR_KEEP_CHARACTER,
	NATIVE_MIRROR_KEEP_RACER_DRIVING,
};
int NativeMirror_KeepUnmirrored(struct Instance *inst);

/* A screen X the game projected itself (GTE) for a flat overlay that marks a world point: the missile
 * lock-on bracket, the lens-flare burst. The renderer mirrors the world but leaves flat overlays alone,
 * so these mirror here, about the GTE's current screen offset. Call while that offset is still the one
 * the point was projected with. */
int NativeMirror_ProjectedX(int sx);

/* Renderer side. NativeRenderer_BeginScene latches NativeMirror_World once per frame, with the packet
 * window's width, so both eyes of a stereo frame use the same answer. The screen mirror is
 * x' = axisSum - x, the packet window's left + right. */
int NativeMirror_LatchFrame(int dispW);
int NativeMirror_WorldFrame(void);
float NativeMirror_AxisSum(void);

#endif /* CTR_NATIVE */

#endif
