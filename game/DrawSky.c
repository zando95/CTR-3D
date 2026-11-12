#include <common.h>

#ifndef CTR_NATIVE_GTE_CAPTURE
#define CTR_NATIVE_GTE_CAPTURE 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
// Declared locally rather than by including platform/native_gte_capture.h:
// this file sits earlier in game_unity.h's include chain than the first file
// that pulls that header in (MAIN/MainFrame.c), and the chain's order is
// load-bearing. Same idiom as the NativeTrackFrame_Record / NativeModelFrame_
// Sample hooks elsewhere in game/. The prototype is checked against the real
// one at the point MainFrame.c does include the header.
extern void GTECapture_SetStoreSuppressed(int suppressed);
// s43: register the skybox packets' address span for the far-depth pin. Same
// local-extern idiom and same header-checked-later guarantee as above.
extern void NativeGpu_MarkSkyRange(uintptr_t start, uintptr_t end);
// Returns nonzero when the platform draws this skybox itself (from its level
// mesh, on the GPU), in which case no packets may be emitted for it.
extern int NativeGpu_SkyGpuSubmit(struct Skybox *sky, struct PushBuffer *pb);
#endif

struct DrawSkyContext
{
	const struct ShortVertex *verts;
	uint32_t *ot;
	u32 screenBounds;
};

struct DrawSkyScratch
{
	u32 baseFaceOffset;
	u32 baseCountOffset;
};

CTR_STATIC_ASSERT(sizeof(struct DrawSkyScratch) == 0x8);
CTR_STATIC_ASSERT(offsetof(struct DrawSkyScratch, baseFaceOffset) == 0x0);
CTR_STATIC_ASSERT(offsetof(struct DrawSkyScratch, baseCountOffset) == 0x4);

static u32 DrawSky_ReadWord(const void *base, int offset)
{
	return *(const u32 *)(const void *)((const char *)base + offset);
}

static int DrawSky_IsVisible(u32 gteFlag, u32 sxy0, u32 sxy1, u32 sxy2, u32 screenBounds)
{
	u32 overlap;
	u32 bounds;

	if (((gteFlag << 13) >> 29) != 0)
	{
		return 0;
	}

	overlap = sxy0 & sxy1 & sxy2;
	bounds = ~((sxy0 - screenBounds) | (sxy1 - screenBounds) | (sxy2 - screenBounds)) | overlap;
	if ((s32)bounds < 0)
	{
		return 0;
	}

	return (s32)(bounds << 16) >= 0;
}

#ifdef CTR_NATIVE
/* s37 widescreen: second-chance visibility for faces the retail packed-SXY
 * test rejected. The retail test culls a face when all three verts sit off
 * the SAME side of the 4:3 rect; while the 3DS wide window is live
 * (NativeWide_CullMarginX nonzero, platform/native_gpu.c) the X sides get
 * the same rectW/8 margin the window gained, so the sky fills the margins
 * instead of ending at the old edge. Plain s16 arithmetic instead of the
 * packed borrow trick -- this path only ever ACCEPTS faces retail rejected,
 * and only while wide, so retail/PC output is untouched by construction.
 * GTE-flag rejects stay absolute (overflowed projections are garbage).
 * NativeWide_MarginClassOn = the strips-hunt kill-switch mask (dumpreq
 * `widemask N`): bit 4 gates this per-face test, bit 8 the extra yaw
 * sectors in DrawSky_Full. */
extern int NativeWide_CullMarginX(int rectW);
extern int NativeWide_MarginClassOn(int bit);

static int DrawSky_IsVisibleWideMargin(u32 gteFlag, u32 sxy0, u32 sxy1, u32 sxy2, u32 screenBounds)
{
	int w = (int)(screenBounds & 0xffff);
	int h = (int)(screenBounds >> 16);
	int margin = NativeWide_MarginClassOn(4) ? NativeWide_CullMarginX(w) : 0;
	s16 x0, x1, x2, y0, y1, y2;

	if (margin == 0)
	{
		return 0;
	}

	if (((gteFlag << 13) >> 29) != 0)
	{
		return 0;
	}

	x0 = (s16)(sxy0 & 0xffff);
	y0 = (s16)(sxy0 >> 16);
	x1 = (s16)(sxy1 & 0xffff);
	y1 = (s16)(sxy1 >> 16);
	x2 = (s16)(sxy2 & 0xffff);
	y2 = (s16)(sxy2 >> 16);

	if ((x0 < -margin) && (x1 < -margin) && (x2 < -margin))
	{
		return 0;
	}
	if ((x0 > w + margin) && (x1 > w + margin) && (x2 > w + margin))
	{
		return 0;
	}
	if ((y0 < 0) && (y1 < 0) && (y2 < 0))
	{
		return 0;
	}
	if ((y0 > h) && (y1 > h) && (y2 > h))
	{
		return 0;
	}

	/* Wrap guard: the GTE saturates screen coords at +/-1024(ish), so a
	 * near-perpendicular face can come back with verts pinned on BOTH clamp
	 * rails -- a wrapped projection that "spans" every side test above and
	 * would draw as a giant garbage triangle (the owner's banded strips on
	 * Polar). A face touching only ONE rail is a legitimate edge-spanner
	 * (its visible part is real; the rasterizer clips the rest) -- rejecting
	 * those would punch holes at the margins, so only the both-rails
	 * signature dies here. */
	{
		int minX = (x0 < x1) ? ((x0 < x2) ? x0 : x2) : ((x1 < x2) ? x1 : x2);
		int maxX = (x0 > x1) ? ((x0 > x2) ? x0 : x2) : ((x1 > x2) ? x1 : x2);

		if ((minX <= -1024) && (maxX >= 1023))
		{
			return 0;
		}
	}

	return 1;
}
#endif

static void DrawSky_LoadFaceVertices(struct DrawSkyContext *ctx, const struct SkyboxFace *face)
{
	const char *verts = (const char *)ctx->verts;
	const struct ShortVertex *a = (const struct ShortVertex *)(const void *)(verts + face->A);
	const struct ShortVertex *b = (const struct ShortVertex *)(const void *)(verts + face->B);
	const struct ShortVertex *c = (const struct ShortVertex *)(const void *)(verts + face->C);

	MTC2(DrawSky_ReadWord(&a->Position, 0x0), 0);
	MTC2(DrawSky_ReadWord(&a->Position, 0x4), 1);
	MTC2(DrawSky_ReadWord(&a->Color, 0x0), 20);

	MTC2(DrawSky_ReadWord(&b->Position, 0x0), 2);
	MTC2(DrawSky_ReadWord(&b->Position, 0x4), 3);
	MTC2(DrawSky_ReadWord(&b->Color, 0x0), 21);

	MTC2(DrawSky_ReadWord(&c->Position, 0x0), 4);
	MTC2(DrawSky_ReadWord(&c->Position, 0x4), 5);
	MTC2(DrawSky_ReadWord(&c->Color, 0x0), 22);
}

static void DrawSky_EmitPrimitive(u32 **primCursor, uint32_t *ot)
{
	POLY_G3 *poly = (POLY_G3 *)*primCursor;

	CtrGpu_WriteColorCode(&poly->r0, MFC2(20));
	CtrGpu_WritePackedXY(&poly->x0, MFC2(12));
	CtrGpu_WriteColorCode(&poly->r1, MFC2(21));
	CtrGpu_WritePackedXY(&poly->x1, MFC2(13));
	CtrGpu_WriteColorCode(&poly->r2, MFC2(22));
	CtrGpu_WritePackedXY(&poly->x2, MFC2(14));
	CtrGpu_LinkPacket24(ot, &poly->tag, poly, 0x06000000);

	*primCursor = (u32 *)(poly + 1);
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80069cc4-0x80069e70
static u32 *DrawSky_Piece(struct Skybox *skybox, struct DrawSkyContext *ctx, int faceIndex, int countIndex, u32 *prim)
{
	u32 numFaces = (u16)skybox->numFaces[countIndex];
	const struct SkyboxFace *face = skybox->ptrFaces[faceIndex];

	if (numFaces == 0)
	{
		return prim;
	}

	for (u32 i = 0; i < numFaces; i++, face++)
	{
		uint32_t *ot = (uint32_t *)(void *)((char *)ctx->ot + (s16)face->D);
		u32 sxy0;
		u32 sxy1;
		u32 sxy2;
		u32 gteFlag;

		DrawSky_LoadFaceVertices(ctx, face);
		gte_rtpt_b();

		sxy0 = MFC2(12);
		gteFlag = CFC2(31);
		sxy1 = MFC2(13);
		sxy2 = MFC2(14);

		int visible = DrawSky_IsVisible(gteFlag, sxy0, sxy1, sxy2, ctx->screenBounds);

#ifdef CTR_NATIVE
		if (visible == 0)
		{
			/* Wide-margin second chance -- see DrawSky_IsVisibleWideMargin. */
			visible = DrawSky_IsVisibleWideMargin(gteFlag, sxy0, sxy1, sxy2, ctx->screenBounds);
		}
#endif

		if (visible != 0)
		{
			DrawSky_EmitPrimitive(&prim, ot);
		}
	}

	return prim;
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80069bb0-0x80069cc4
void DrawSky_Full(void *skybox, struct PushBuffer *pb, struct PrimMem *primMem)
{
	struct Skybox *sky = skybox;
	u32 *prim = (u32 *)primMem->cursor;
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s43: the emitted packets run contiguously from here; the span is closed
	 * and registered after the last DrawSky_Piece below. */
	uintptr_t skyPrimStart = (uintptr_t)prim;

	if (NativeGpu_SkyGpuSubmit(sky, pb))
	{
		return;
	}
#endif

	// NOTE(aalhendi): PSX-backfeed blocker: retail saves/restores ra and s0-s2 in scratchpad 0x00-0x0c.
	// Native C relies on the host ABI because the only retail data temporaries live in 0x10 and 0x14 and are explicit below.
	if (sky != NULL)
	{
		struct DrawSkyScratch *scratch = CTR_SCRATCHPAD_PTR(struct DrawSkyScratch, 0x10);
		struct DrawSkyContext ctx;
		u32 baseFaceOffset;
		u32 baseCountOffset;
		int faceIndex;
		int countIndex;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		// The three CTC2(0, 5..7) below zero the GTE's translation vector: the
		// skybox rotates with the camera and never translates, because it is
		// meant to be at infinity. That makes every SZ3 this function produces a
		// function of the sky mesh's own radius rather than of distance, so
		// nothing downstream may treat those depths as real. Suppress the
		// capture stores for the whole projection and let the packet-depth path
		// derive depth from the OT bucket instead -- which is retail's own
		// statement of how far this content is. Full reasoning, including the
		// second failure mode (sky vertices EVICTING world vertices from a table
		// keyed by screen coordinate), is on GTECapture_SetStoreSuppressed.
		GTECapture_SetStoreSuppressed(1);
#endif

		CTC2(DrawSky_ReadWord(&pb->matrix_ViewProj, 0x00), 0);
		CTC2(DrawSky_ReadWord(&pb->matrix_ViewProj, 0x04), 1);
		CTC2(DrawSky_ReadWord(&pb->matrix_ViewProj, 0x08), 2);
		CTC2(DrawSky_ReadWord(&pb->matrix_ViewProj, 0x0c), 3);
		CTC2(DrawSky_ReadWord(&pb->matrix_ViewProj, 0x10), 4);
		CTC2(0, 5);
		CTC2(0, 6);
		CTC2(0, 7);

		baseFaceOffset = ((DrawSky_ReadWord(pb, 0x08) + 0x500) >> 7) & 0x1c;
		baseCountOffset = (baseFaceOffset >> 1) & 0xe;

		scratch->baseFaceOffset = baseFaceOffset;
		scratch->baseCountOffset = baseCountOffset;

		ctx.verts = sky->ptrVertex;
		ctx.ot = &pb->ptrOT[0x3ff];
		ctx.screenBounds = DrawSky_ReadWord(pb, 0x20);

		faceIndex = (int)(baseFaceOffset >> 2);
		countIndex = (int)(baseCountOffset >> 1);
		prim = DrawSky_Piece(sky, &ctx, faceIndex, countIndex, prim);

		baseFaceOffset = (baseFaceOffset + 4) & 0x1c;
		baseCountOffset = (baseCountOffset + 2) & 0xe;
		faceIndex = (int)(baseFaceOffset >> 2);
		countIndex = (int)(baseCountOffset >> 1);
		prim = DrawSky_Piece(sky, &ctx, faceIndex, countIndex, prim);

		baseFaceOffset = (scratch->baseFaceOffset - 4) & 0x1c;
		baseCountOffset = (scratch->baseCountOffset - 2) & 0xe;
		faceIndex = (int)(baseFaceOffset >> 2);
		countIndex = (int)(baseCountOffset >> 1);
		prim = DrawSky_Piece(sky, &ctx, faceIndex, countIndex, prim);

		baseFaceOffset = (scratch->baseFaceOffset - 8) & 0x1c;
		baseCountOffset = (scratch->baseCountOffset - 4) & 0xe;
		faceIndex = (int)(baseFaceOffset >> 2);
		countIndex = (int)(baseCountOffset >> 1);
		prim = DrawSky_Piece(sky, &ctx, faceIndex, countIndex, prim);

#ifdef CTR_NATIVE
		/* s37 widescreen: the retail 4-of-8 yaw-sector window (+1 back to -2
		 * around the camera) has enough slack for the 4:3 FOV at any yaw
		 * phase, but the wide window can poke past its thin side -- owner
		 * saw the sky ending mid-margin on one edge only, which is exactly
		 * a sector boundary, not a face cull. While wide is live, draw one
		 * extra sector on each side (+2 and -3); DrawSky_IsVisibleWideMargin
		 * still rejects their faces per-face, so the emitted extras are only
		 * what the margins actually show. The 512 argument is nominal -- the
		 * call is a mode probe (nonzero exactly when the wide window is
		 * live). */
		if (NativeWide_MarginClassOn(8) && (NativeWide_CullMarginX(512) != 0))
		{
			baseFaceOffset = (scratch->baseFaceOffset + 8) & 0x1c;
			baseCountOffset = (scratch->baseCountOffset + 4) & 0xe;
			faceIndex = (int)(baseFaceOffset >> 2);
			countIndex = (int)(baseCountOffset >> 1);
			prim = DrawSky_Piece(sky, &ctx, faceIndex, countIndex, prim);

			baseFaceOffset = (scratch->baseFaceOffset - 12) & 0x1c;
			baseCountOffset = (scratch->baseCountOffset - 6) & 0xe;
			faceIndex = (int)(baseFaceOffset >> 2);
			countIndex = (int)(baseCountOffset >> 1);
			prim = DrawSky_Piece(sky, &ctx, faceIndex, countIndex, prim);
		}
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
		// Cleared inside the same `if (sky != NULL)` block that set it, and
		// after the last DrawSky_Piece, so the flag can never outlive this
		// call -- there is no early return between the two.
		GTECapture_SetStoreSuppressed(0);
#endif
	}

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/* s43: close and register the emitted span [start, cursor). When sky==NULL
	 * nothing advanced, so this is an empty range and NativeGpu_IsSkyPacket
	 * matches nothing -- the same no-op the suppression bracket takes. */
	NativeGpu_MarkSkyRange(skyPrimStart, (uintptr_t)prim);
#endif

	primMem->cursor = prim;
}
