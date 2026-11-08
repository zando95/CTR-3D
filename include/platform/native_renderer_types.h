/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/include/PsyX/PsyX_render.h
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#ifndef NATIVE_RENDERER_TYPES_H
#define NATIVE_RENDERER_TYPES_H

#include <macros.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>

#define LUT_WIDTH              (256)
#define LUT_HEIGHT             (256)

#define VRAM_WIDTH             (1024)
#define VRAM_HEIGHT            (512)

#define TPAGE_WIDTH            (256)
#define TPAGE_HEIGHT           (256)

#define MAX_VERTEX_BUFFER_SIZE (1u << 16)

#pragma pack(push, 1)
typedef struct
{
	s16 x, y, page, clut;

	u8 u, v, bright, dither;
	u8 r, g, b, a;

	s8 tcx, tcy, _p0, _p1;

	/*
	 * Per-primitive emission ordinal, stamped in OT-walk order by the parse
	 * layer (native_gpu.c). All vertices of one primitive share a value;
	 * later primitives get larger ones. This is the depth substrate for the
	 * 3DS z-break play: submission order can be changed without changing
	 * layering, because depth = ordinal reproduces painter's order in the
	 * depth buffer. Appended field: PC's GL attribute loaders address fields
	 * by offset and never fetch it, so PC output is unaffected.
	 *
	 * OCCLUSION channel. Since the split below, this one and only this one
	 * decides what covers what.
	 */
	u16 emitOrdinal;

	/*
	 * PARALLAX channel. Owner's framing:
	 * "game world depth and 3d output depth don't need to be the same."
	 *
	 * Normally identical to emitOrdinal -- the depth a thing occludes at IS
	 * the depth it should appear at in stereo. The menu/loading checkered
	 * flag is the exception this exists for: retail's own layering wants it
	 * painted over everything mid-wipe and behind the menu models when
	 * parked (a per-pose OCCLUSION answer), while the stereo eye wants its
	 * wave's true relief continuously, in every pose, with no jump (one
	 * fixed PARALLAX band just behind the convergence plane). One depth
	 * field cannot say both; two can.
	 *
	 * Same units and axis as emitOrdinal (nearer = larger), so 3DS biases
	 * both into s16 identically and the vertex shader's d = 32767 - value
	 * inverse works unchanged on it. Appended for the same reason and with
	 * the same PC guarantee as emitOrdinal above: no GL loader fetches it.
	 */
	u16 parallaxOrdinal;
} GrVertex;
#pragma pack(pop)

/* UI-band stamp for the in-race minimap block: still inside the >= 0xFFC0 UI
 * classifier band, one below the ordered-UI pin (0xFFFF), so the stereo
 * flatten and the widescreen X ramp keep treating it as UI. Stamped whenever
 * the ordered-UI pin owns the band, NOT only while the bottom-screen map is
 * routed: it is both the peel key for the bottom panel (that consumer carries
 * the dumpreq 'btmmap' gate itself) and the sprite-class key for the far-edge
 * UV inset that closes the seam between the map sheet's two half-icons on
 * either screen. Producer: PacketDepth_ResolveXY (native_gpu.c); consumers:
 * NativeRenderer_DrawTriangles' UI branch and N3DS_ConvertTriangleVerts
 * (native_renderer_3ds.c). Only ever stamped while the pin owns the band, so
 * every other stamp consumer still sees exactly {band, 0xFFFF}. */
#define PKTDEPTH_UI_STAMP_BTMMAP 0xFFFDu
/* Same mechanism, second class: the FPS overlay text inside the map block.
 * Peeled separately so the bottom pass can draw it through its OWN ortho
 * window (small, top-left, ~top-screen scale) instead of the map's
 * magnifying one. Deliberately NEARER than the map's stamp: the text draws
 * last and overlays the map's panel pixels, so it must win the LEQUAL test
 * against the map's written depth. */
#define PKTDEPTH_UI_STAMP_BTMFPS 0xFFFEu
/* Third class: picture sprites that want ONLY the seam-closing far-edge UV
 * inset (N3DS_ConvertTriangleVerts) and are never routed off the top
 * screen -- the track-select and adventure-hub map sheets. The renderer
 * re-pins their z/w to the 0xFFFF pin at the ordered-UI append: ordered-UI
 * layering is draw-order-under-EQUAL-depth, and an off-pin stamp there
 * would let earlier-drawn quad rects occlude these sprites. Never stamp
 * 1:1 glyph text with this class -- the inset's gradient change duplicates
 * rows on 1:1 content. */
#define PKTDEPTH_UI_STAMP_UIPIC 0xFFFCu
/* Fourth class: the juiced-wumpa HUD shine (UI_Weapon_DrawBG's four mirrored
 * quadrant quads behind the fruit and the held item). Retail links them into
 * the PLAYER's OT, not the UI OT, so without its span mark the resolver misses
 * them onto a world-bucket stamp -- outside the UI band, so no widescreen X
 * ramp and no stereo flatten, while the fruit and icon they sit behind take
 * both. Re-pinned to 0xFFFF at the ordered-UI append exactly like UIPIC. Also
 * keys the far-edge UV inset on every axis: the quadrants meet at their far
 * edges, so the past-edge texel band reads as a seam line through the burst
 * centre. Retail's one-unit mirrored overlap (which hid the PS1's past-edge
 * first column) draws that seam column twice under this inset, so the game
 * drops it while NativeUiShine_ExactTiling reports the inset live. */
#define PKTDEPTH_UI_STAMP_HUDSHINE 0xFFFBu
/* Fifth class: retail's full-window fade quad (PushBuffer_FadeOneWindow -- the
 * white flash of a hit or the finish line, and the fades to and from black).
 * The per-player quads are linked into the PLAYER's OT, so without their span
 * mark they miss onto a world-bucket stamp: depth-tested against the track and
 * models, which then cut through the flash, and never widened into the
 * widescreen margins. Stamped into the band they draw in the ordered UI pass
 * over the whole world and under the HUD, exactly retail's painter order.
 * Re-pinned to 0xFFFF at the ordered-UI append like UIPIC; the stamp itself
 * keys the screen-edge overscan in N3DS_ConvertTriangleVerts, which pushes
 * window-edge vertices past the wide window by the stereo pad so neither eye's
 * UI pop-out can pull an edge on-screen. */
#define PKTDEPTH_UI_STAMP_SCREENFADE 0xFFFAu

typedef enum
{
	a_position,
	a_texcoord,
	a_color,
	a_extra,
} ShaderAttrib;

typedef enum
{
	BM_NONE,
	BM_AVERAGE,
	BM_ADD,
	BM_SUBTRACT,
	BM_ADD_QUATER_SOURCE
} BlendMode;

typedef enum
{
	TF_4_BIT,
	TF_8_BIT,
	TF_16_BIT,

	TF_32_BIT_RGBA
} TexFormat;

typedef u32 TextureID;
typedef u32 ShaderID;

#endif
