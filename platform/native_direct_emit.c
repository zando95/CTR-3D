/*
 * Direct-emit fast path (retained-geometry campaign, transitional form). For an OT node whose payload is
 * nothing but opaque poly prims, emits packet -> merge bin directly, skipping
 * the GrVertex/split path. Byte-parity with the normal path is the design
 * invariant (see DirectEmit_WalkNode). A node with any draw-env prim,
 * non-poly prim, semi-trans poly, TF_16_BIT textured poly (framebuffer
 * feedback), or malformed tiling stays on the normal path. 3DS only: the PC
 * build does not compile this file's body.
 *
 * This file is part of the main.c unity TU and is included AFTER
 * native_gpu.c and platform/n3ds/native_renderer_3ds.c so it can see their
 * file-scope state (s_gpu, activeDrawEnv, merge bins, the decode-cache
 * resolve).
 */

#ifndef CTR_NATIVE_DIRECT_EMIT
#define CTR_NATIVE_DIRECT_EMIT 0
#endif

#if defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT

/* Node-skip reasons, one per node (first disqualifier wins). */
enum
{
	DEMIT_OK = 0,
	DEMIT_SKIP_ENV,
	DEMIT_SKIP_NONPOLY,
	DEMIT_SKIP_BLEND,
	DEMIT_SKIP_FMT16,
	DEMIT_SKIP_MALFORMED,
	DEMIT_SKIP_COUNT
};

typedef struct
{
	/* per-frame, reset by DirectEmit_FoldFrame */
	u32 frameNodes;
	u32 frameElig;
	u32 framePrims;
	u32 frameTris;
	u32 frameNullPrims;
	u32 frameEmitTris;
	u32 frameSkip[DEMIT_SKIP_COUNT];

	/* window accumulators, printed every 64 content frames */
	u32 winFrames;
	u32 winNodes;
	u32 winElig;
	u32 winPrims;
	u32 winTris;
	u32 winNullPrims;
	u32 winEmitTris;
	u32 winSkip[DEMIT_SKIP_COUNT];
} DirectEmitStats;

global_variable DirectEmitStats s_demit;

#define DEMIT_WINDOW_FRAMES 64

/*
 * One walker, two passes: emit=0 validates the whole node and counts content
 * (pure -- no state touched), emit=1 re-walks and emits. Sharing the walk
 * keeps the layout tables single-copy; the dry pass guarantees the emit pass
 * cannot encounter a disqualifier after side effects have started.
 * Word layouts mirror ProcessFlatPoly/ProcessGouraudPoly exactly; a node
 * whose prim lengths do not tile tagLength is rejected, same guard as the
 * [OTPROF] shape walker.
 */
internal int DirectEmit_WalkNode(const u32 *words, u32 len, int emit)
{
	u32 w = 0;

	float ofsX = 0.0f, ofsY = 0.0f;

	if (emit)
	{
		/* Once per node: env prims are excluded, so the offset cannot
		 * change mid-node. Float on purpose -- MakeVertexTriangle/Quad add
		 * this exact float to the s16 packet coordinate, and byte-parity
		 * beats saving two VFP converts per vertex. */
		DrawEnvOffset(&ofsX, &ofsY);
	}

	while (w < len)
	{
		const u32 cmd = words[w];
		const u32 code = cmd >> 24;

		if ((code < 0x20u) || (code > 0x3Fu))
		{
			return ((code >= 0xE0u) && (code <= 0xE7u)) ? DEMIT_SKIP_ENV : DEMIT_SKIP_NONPOLY;
		}
		if (code & 0x02u)
		{
			return DEMIT_SKIP_BLEND;
		}

		{
			const int gouraud = (code & 0x10u) != 0;
			const int quadPrim = (code & 0x08u) != 0;
			const int textured = (code & 0x04u) != 0;
			const int numVerts = quadPrim ? 4 : 3;
			const u32 primLen = 1u + (u32)numVerts * (textured ? 2u : 1u) + (gouraud ? (u32)(numVerts - 1) : 0u);
			u32 xy[4], uvw[4], rgb[4];
			u32 c = w + 1;
			int v;

			if (w + primLen > len)
			{
				return DEMIT_SKIP_MALFORMED;
			}

			uvw[0] = uvw[1] = uvw[2] = uvw[3] = 0;
			rgb[0] = cmd & 0x00FFFFFFu;
			for (v = 0; v < numVerts; v++)
			{
				if (v > 0)
				{
					rgb[v] = gouraud ? (words[c++] & 0x00FFFFFFu) : rgb[0];
				}
				xy[v] = words[c++];
				if (textured)
				{
					uvw[v] = words[c++];
				}
			}

			/* tpage rides the second vertex's UV word, clut the first --
			 * the POLY_FT/GT struct layout. */
			if (textured && (GetTPageFormat((int)(s16)(uvw[1] >> 16)) == TF_16_BIT))
			{
				return DEMIT_SKIP_FMT16;
			}

			if (!emit)
			{
				s_demit.framePrims++;
				if (!gouraud && textured && !quadPrim && (xy[0] == 0xFFFFFFFFu) && (xy[1] == 0xFFFFFFFFu) && (xy[2] == 0xFFFFFFFFu))
				{
					s_demit.frameNullPrims++;
				}
				else
				{
					s_demit.frameTris += quadPrim ? 2u : 1u;
				}
			}
			else
			{
				const s16 rawTPage = (s16)(uvw[1] >> 16);
				const int isNullFT3 =
				    (!gouraud && textured && !quadPrim && (xy[0] == 0xFFFFFFFFu) && (xy[1] == 0xFFFFFFFFu) && (xy[2] == 0xFFFFFFFFu));
				u16 ordinal;
				s16 z;
				s16 zPerVert[4];
				/* Parallax channel twin:
				 * same indices, same validity flag -- the resolver fills both
				 * or neither. */
				s16 wPerVert[4];
				int zPerVertValid;

				/* Side effect order matches ProcessFlat/GouraudPoly: the
				 * tpage write happens before anything else, null or not. */
				if (textured)
				{
					activeDrawEnv.tpage = rawTPage;
				}

				/* One stamp per prim, unconditionally -- the normal path's
				 * StampEmitOrdinal advances even for the null-FT3 DR_TPAGE
				 * hack (it stamps zero vertices, then increments). Same
				 * shared stamp source as the parse path, so the ordinal
				 * widening scheme (dumpreq 'otz') covers direct emit too. */
				ordinal = NativeGpu_NextDepthStamp();
				s_gpu.emitOrdinal++;
				/* Depth-compose fix (native_gpu.c's NativeGpu_RecordContentAdvance
				 * comment): this fast path advances the emit ordinal exactly
				 * like the normal parse path's two NativeGpu_StampEmitOrdinal
				 * call sites, so it must feed minContentBucket the same way --
				 * otherwise every direct-emitted node (most of a heavy track's
				 * opaque content) would be invisible to the depth-compose
				 * floor, and the fix would only work when direct emit happens
				 * to be off. */
				NativeGpu_RecordContentAdvance();
				z = (s16)((int)ordinal - 32768);
				/* Per-vertex depth. Stays invalid -- and every vertex
				 * keeps the flat z above -- unless the resolver below fills it,
				 * so the feature-off and fallback paths are byte-unchanged. */
				zPerVertValid = 0;

#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
				/* Depth model. This path
				 * writes its own z and handles most of a heavy track's opaque
				 * content, so it MUST resolve depth exactly as the parse path
				 * does -- if only one of the two moves onto the true-depth
				 * axis, the halves of the same frame end up on different
				 * scales. Returns 0 with the feature off, leaving the ordinal
				 * above untouched. */
				{
					u16 depthStamp;
					u16 depthPerVert[4];
					u16 depthPerVertPar[4];

					if (PacketDepth_ResolvePacked(xy, numVerts, &depthStamp, depthPerVert, depthPerVertPar))
					{
						int v;

						z = (s16)((int)depthStamp - 32768);
						/* Per-vertex depth: keep each corner's own depth so
						 * the GPU interpolates across the polygon, instead of
						 * flattening the whole primitive onto its FARTHEST
						 * corner (retail's OT-bucket convention, which loses to
						 * the resident track's real interpolated depth wherever
						 * the two meet -- the road drawing over Crash). Indexed
						 * by SOURCE vertex below via idx[], so triangulation
						 * order cannot pair a vertex with a stranger's depth. */
						for (v = 0; v < numVerts && v < 4; v++)
						{
							zPerVert[v] = (s16)((int)depthPerVert[v] - 32768);
							wPerVert[v] = (s16)((int)depthPerVertPar[v] - 32768);
						}
						zPerVertValid = 1;
					}
				}
#endif

				if (!isNullFT3)
				{
					const int noShade = (!gouraud && (code & 0x01u));
					const s16 texPage = textured ? GetTPageBase((int)rawTPage) : 0;
					const s16 clut = textured ? (s16)(uvw[0] >> 16) : 0;
					const TexFormat fmt = textured ? GetTPageFormat((int)rawTPage) : TF_4_BIT;
					const int numTris = quadPrim ? 2 : 1;
					int t;

					/* Packet-index triangles (0,1,2) and (1,2,3): what
					 * MakeVertexQuad's x0,x1,x3,x2 argument order and
					 * TriangulateQuad's element swaps net out to. */
					for (t = 0; t < numTris; t++)
					{
						const int i0 = t, i1 = t + 1, i2 = t + 2;
						int bin, atlasIdx = 0, pbU = 0, pbV = 0, oU = 0, oV = 0;
						N3DS_Vertex *dst;
						int k;
						const int idx[3] = {i0, i1, i2};

						if (textured)
						{
							/* Resolve runs per frame per triangle exactly as
							 * the normal path's DrawTriangles loop does --
							 * decode-on-miss, LRU touch, fb-mirror refresh,
							 * sub-page/full-page routing all preserved. Only
							 * .u/.v of these are read (see
							 * N3DS_ResolveTriangleTexture). */
							GrVertex tv[3];

							for (k = 0; k < 3; k++)
							{
								tv[k].u = (u8)(uvw[idx[k]] & 0xFFu);
								tv[k].v = (u8)((uvw[idx[k]] >> 8) & 0xFFu);
							}
							N3DS_ResolveTriangleTexture(&tv[0], &tv[1], &tv[2], texPage, clut, fmt, N3DS_TEXVIEW_OPAQUE, &atlasIdx, &pbU, &pbV, &oU,
							                            &oV);
							bin = atlasIdx;
						}
						else
						{
							bin = N3DS_MERGE_UNTEX_BIN;
						}

						/* Sky-first: the skybox (tagged by s_skyNodeActive during the walk,
						 * native_gpu.c) accumulates into its own buffer so N3DS_DrawSkyPass can
						 * draw it before the track pass -- otherwise, binned as ordinary opaque,
						 * it flushes after the track and paints over the depth-less waterfall. */
						if (s_skyNodeActive)
						{
							if ((s_skyVertUsed + 3) > N3DS_SKY_VERT_CAP)
							{
								continue;
							}
							dst = &s_skyVerts[s_skyVertUsed];
						}
						else
						{
							if ((s_mergeBinUsed[bin] + 3) > N3DS_MERGE_BIN_CAP_VERTS)
							{
								N3DS_MergeSpillBin(bin);
							}
							dst = &s_mergeBinVerts[bin][s_mergeBinUsed[bin]];
						}

						for (k = 0; k < 3; k++)
						{
							const u32 xyWord = xy[idx[k]];
							const u32 col = rgb[idx[k]];
							s16 dx = (s16)((float)(s16)(xyWord & 0xFFFFu) + ofsX);
							const s16 dz = zPerVertValid ? zPerVert[idx[k]] : z;

							/* Widescreen UI relocation -- same rule as N3DS_ConvertTriangleVerts's:
							 * UI-band stamps (biased z >= 32704 = raw 0xFFC0) get the X ramp
							 * (NativeWide_UiShiftX). zPerVertValid doubles as the packet-depth-live
							 * gate: an ordinal z cannot reach the band. No-op with wide off, so
							 * PC/[DEMIT] goldens are untouched. */
							if (zPerVertValid && (dz >= 32704))
							{
								extern int NativeWide_UiShiftX(int x);

								dx = (s16)NativeWide_UiShiftX((int)dx);
							}
							/* s46: the horizon gradient is screen-space geometry hardcoded
							 * to the retail 512-wide rect; stretch it to the wide ortho
							 * window (identity with wide off). Glow slice only -- see
							 * NativeWide_GlowStretchX for why the dome must not take this.
							 * Mutually exclusive with the UI ramp above by depth: glow is
							 * far-pinned. */
							if (s_glowNodeActive)
							{
								extern int NativeWide_GlowStretchX(int x);

								dx = (s16)NativeWide_GlowStretchX((int)dx);
							}
							/* Menu backdrop: same ramp as the window border these
							 * strips sit inside, which is UI and already takes it --
							 * see MakeVertexQuad's copy. Mutually exclusive with the
							 * UI branch above by depth (the backdrop is midband). */
							if (s_menubdNodeActive)
							{
								extern int NativeWide_UiShiftX(int x);

								dx = (s16)NativeWide_UiShiftX((int)dx);
							}
							dst[k].x = dx;
							dst[k].y = (s16)((float)(s16)(xyWord >> 16) + ofsY);
							dst[k].z = dz;
							/* Parallax channel: the resolver's second
							 * output where it exists, otherwise the same value
							 * as z -- an ordinal-stamped or unresolved prim has
							 * no separate stereo depth to carry. */
							dst[k].w = zPerVertValid ? wPerVert[idx[k]] : dz;
							if (textured)
							{
								const int srcU = (int)(uvw[idx[k]] & 0xFFu);
								const int srcV = (int)((uvw[idx[k]] >> 8) & 0xFFu);

								dst[k].u = (s16)(((pbU + (srcU - oU)) << 1) + 1);
								dst[k].v = (s16)(2047 - ((pbV + (srcV - oV)) << 1));
							}
							else
							{
								dst[k].u = (s16)(((N3DS_WHITE_CELL_X * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1) + 1);
								dst[k].v = (s16)(2047 - ((N3DS_WHITE_CELL_Y * N3DS_DECODE_TEXTURE_DIM + (N3DS_DECODE_TEXTURE_DIM / 2)) << 1));
							}
							if (noShade)
							{
								dst[k].r = 128;
								dst[k].g = 128;
								dst[k].b = 128;
							}
							else
							{
								dst[k].r = (u8)(col & 0xFFu);
								dst[k].g = (u8)((col >> 8) & 0xFFu);
								dst[k].b = (u8)((col >> 16) & 0xFFu);
							}
						}
						/* Advance whichever buffer this tri went to. */
						if (s_skyNodeActive)
						{
							s_skyVertUsed += 3;
						}
						else
						{
							s_mergeBinUsed[bin] += 3;
						}
						s_demit.frameEmitTris++;
					}
				}
			}

			w += primLen;
		}
	}

	if (emit)
	{
		/* Every fast-path prim is non-feedback, and AddSplit's non-feedback
		 * branch clears this flag per prim -- keep the feedback-run state
		 * machine seeing the same sequence it would have. */
		s_gpu.framebufferFeedbackRunActive = false;
	}

	return DEMIT_OK;
}

/*
 * Called from ParsePrimitivesLinkedList per OT node (after the tagLength
 * validity checks, before the parse loop). Returns 1 when the node was fully
 * emitted here and the parse loop must be skipped; 0 means parse normally
 * (ineligible node, fast path inactive, or PC count-only build).
 */
internal int NativeDirectEmit_TryNode(const u8 *packet, u32 tagLength)
{
	const u32 *words = (const u32 *)packet + P_LEN;
	int active;
	int scan;

	active = (s_gpu.overrideTexture == 0) && !s_gpu.drawPrimMode && !g_dbg_texturelessMode && !g_dbg_forceSingleTexture && (g_dbg_atlasView == 0);
#ifdef CTR_INTERNAL
	/* An armed triangle dump observes the normal path's DrawTriangles loop;
	 * fall back for that frame so its record set stays complete. */
	active = active && (s_triDumpFile == NULL);
#endif
#if defined(CTR_NATIVE_VERTEX_DESNAP) && CTR_NATIVE_VERTEX_DESNAP
	/* Desnap carries sub-pixel fractions in GrVertex padding; the fast path
	 * has no GrVertex to carry them in. */
	active = 0;
#endif
#if defined(CTR_NATIVE) && CTR_NATIVE_GTE_CAPTURE
	/*
	 * Stand aside for 2D/UI nodes so UI has exactly one producer. The ordered UI
	 * list is only correct if appends arrive in OT-walk order, and the two
	 * producing paths do not interleave: direct emit writes vertices DURING the
	 * parse, while the packet path's prims replay later from splits (DrawOTag
	 * parses, then DrawAllSplits runs) -- so a list fed by both is ordered
	 * [all direct-emit UI][all packet UI], not walk order. Declining the node
	 * routes it through the packet path instead, which preserves walk order
	 * within itself. Unconditional (not gated on g_dbg_uiOrder): the same
	 * two-producer defect breaks UI either way. s_gteCaptureSuppress is the same
	 * predicate PacketDepth_ResolveXY uses for the UI band, so the two paths
	 * cannot disagree about what counts as UI. The sky is not caught here
	 * because the suppress flag latches only once the walk enters the UI
	 * pushbuffer address band, and the sky walks before it.
	 */
	/* The HUD shine and full-window fade spans join the predicate here exactly
	 * as they do in the resolver, so the two paths keep agreeing about what
	 * counts as UI. */
	if (GTECapture_LookupSuppressed() || (g_dbg_hudShine && s_hudshineNodeActive) || (g_dbg_screenFade && s_screenfadeNodeActive))
	{
		active = 0;
	}
#endif

	scan = DirectEmit_WalkNode(words, tagLength, 0);
	s_demit.frameNodes++;
	if (scan != DEMIT_OK)
	{
		s_demit.frameSkip[scan]++;
		return 0;
	}
	s_demit.frameElig++;

	if (!active)
	{
		return 0;
	}

	/* One scope per handled node (~1-1.5k/frame in a race) under the same
	 * runtime flag as the other per-call scopes: splits the moved emit work
	 * out of draw_otag_parse_ms (the finding this bucket exists for). */
	if (g_dbg_perfTriScopes)
	{
		NativePerf_BeginScope(NATIVE_PERF_BUCKET_DIRECT_EMIT);
		DirectEmit_WalkNode(words, tagLength, 1);
		NativePerf_EndScope(NATIVE_PERF_BUCKET_DIRECT_EMIT);
		return 1;
	}

	DirectEmit_WalkNode(words, tagLength, 1);
	return 1;
}

/*
 * Per content-frame fold + 64-frame window report, called from ClearSplits
 * (same cadence as [OTPROF]). Printed via Platform_Log so it reaches the 3DS
 * SD log -- the [OTPROF] console-only gap is exactly what this
 * line must not repeat, PC replays and hardware need the same instrument.
 */
internal void DirectEmit_FoldFrame(void)
{
	int i;

	if (s_demit.frameNodes == 0)
	{
		return;
	}

	s_demit.winFrames++;
	s_demit.winNodes += s_demit.frameNodes;
	s_demit.winElig += s_demit.frameElig;
	s_demit.winPrims += s_demit.framePrims;
	s_demit.winTris += s_demit.frameTris;
	s_demit.winNullPrims += s_demit.frameNullPrims;
	s_demit.winEmitTris += s_demit.frameEmitTris;
	for (i = 0; i < DEMIT_SKIP_COUNT; i++)
	{
		s_demit.winSkip[i] += s_demit.frameSkip[i];
	}

	s_demit.frameNodes = 0;
	s_demit.frameElig = 0;
	s_demit.framePrims = 0;
	s_demit.frameTris = 0;
	s_demit.frameNullPrims = 0;
	s_demit.frameEmitTris = 0;
	for (i = 0; i < DEMIT_SKIP_COUNT; i++)
	{
		s_demit.frameSkip[i] = 0;
	}

	if (s_demit.winFrames < DEMIT_WINDOW_FRAMES)
	{
		return;
	}

	/* N3DS_DebugLog, NOT Platform_Log: on real hardware the Platform_Log app
	 * log file never gets created (netloaded runs have no writable cwd for
	 * the relative fopen), so its lines exist nowhere pullable -- measured on
	 * first netload: Azahar had the [DEMIT] stream, the console had
	 * none. Same gap class as [OTPROF]; ctr3ds.log is the sink that works. */
	{
		char line[224];

		snprintf(line, sizeof(line), "[DEMIT] f=%lu nodes=%lu elig=%lu (%lu%%) prims=%lu tris=%lu null=%lu emit=%lu skip env=%lu poly=%lu blend=%lu fmt16=%lu mal=%lu",
		         (unsigned long)s_demit.winFrames, (unsigned long)s_demit.winNodes, (unsigned long)s_demit.winElig,
		         (unsigned long)((s_demit.winNodes != 0) ? ((s_demit.winElig * 100u) / s_demit.winNodes) : 0u), (unsigned long)s_demit.winPrims,
		         (unsigned long)s_demit.winTris, (unsigned long)s_demit.winNullPrims, (unsigned long)s_demit.winEmitTris,
		         (unsigned long)s_demit.winSkip[DEMIT_SKIP_ENV], (unsigned long)s_demit.winSkip[DEMIT_SKIP_NONPOLY],
		         (unsigned long)s_demit.winSkip[DEMIT_SKIP_BLEND], (unsigned long)s_demit.winSkip[DEMIT_SKIP_FMT16],
		         (unsigned long)s_demit.winSkip[DEMIT_SKIP_MALFORMED]);
		N3DS_DebugLog(line);
	}

	memset(&s_demit, 0, sizeof(s_demit));
}

#endif /* defined(CTR_NATIVE) && CTR_NATIVE_DIRECT_EMIT */
