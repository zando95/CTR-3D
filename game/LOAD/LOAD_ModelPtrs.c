#include <common.h>

#if defined(CTR_NATIVE)

// Oxide's size and exhaust pipes, per craft (include/oxide_fit.h).
//
// The model file is shared by every Oxide on the track, so nothing here edits it: the size a craft is drawn at
// is applied where the renderer builds that instance's matrix, and these functions say what that size is.

// The pipe tops of every animation frame, built once from the model's own vertex stream. A frame is stored as
// the sum of the four pipe-top vertices of each pipe, in the GTE vertex units the renderer feeds the GTE.
struct OxideFitPipeFrame
{
	s16 sum[2][3];
};

enum
{
	OXIDE_FIT_PIPE_VERT_COUNT = 4,
	OXIDE_FIT_COMMAND_LIST_CAP = 4096,
	OXIDE_FIT_COMMAND_LIST_END = -1,
	OXIDE_FIT_CACHED_VERTEX_BIT = 0x04000000,
	OXIDE_FIT_ANIM_INTERP_BIT = 0x8000,
	OXIDE_FIT_ANIM_FRAME_MASK = 0x7fff,
	OXIDE_FIT_GTE_VERTEX_SHIFT = 2,
	OXIDE_FIT_FRAME_ORIGIN_X_MASK = 0x7fff,
	OXIDE_FIT_VERTEX_Y_MASK = 0xfff8,
	OXIDE_FIT_DELTA_FULL_BITS = 7,
	OXIDE_FIT_HEADER_SCALE_SHIFT = 2,
	OXIDE_FIT_EMITTER_SHIFT_ONE_FRAME = 6,
	OXIDE_FIT_EMITTER_SHIFT_TWO_FRAMES = 7,
};

static struct OxideFitPipeFrame s_oxfitFrames[OXIDE_FIT_PIPE_MAX_ANIMS][OXIDE_FIT_PIPE_MAX_FRAMES];
static u16 s_oxfitAnimNumFrames[OXIDE_FIT_PIPE_MAX_ANIMS];
static u8 s_oxfitAnimStored[OXIDE_FIT_PIPE_MAX_ANIMS];
static const struct ModelHeader *s_oxfitHeader;
static const void *s_oxfitHeaderAnims;
static u32 s_oxfitHeaderCommands;
static b32 s_oxfitUsable;

static u32 OxideFit_ReadLE32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static s32 OxideFit_SignExtend(u32 value, s32 bits)
{
	s32 shift = 32 - bits;
	return (s32)(value << shift) >> shift;
}

// The renderer's MSB-first bit reader over little-endian words (RenderBucket_GetSignedBits).
static s32 OxideFit_ReadBits(const u8 *data, s32 *bitIndex, s32 bits)
{
	s32 word = *bitIndex >> 5;
	s32 shift = (32 - bits) - (*bitIndex & 31);
	u32 value = OxideFit_ReadLE32(data + (word * 4));

	if (shift < 0)
	{
		value = (value << -shift) | (OxideFit_ReadLE32(data + ((word + 1) * 4)) >> (shift & 31));
	}
	else
	{
		value >>= shift;
	}

	*bitIndex += bits;
	return OxideFit_SignExtend(value, bits);
}

// Decodes one frame's vertices 0..count-1 the way RenderBucket_UncompressAnimationFrame does (delta stream or
// plain bytes, the frame origin added, times four) and stores each as the three GTE vertex words.
static void OxideFit_DecodeFrame(const struct ModelAnim *anim, const struct ModelFrame *frame, s32 count, s16 (*out)[3])
{
	const u8 *stream = (const u8 *)frame + frame->vertexOffset;
	const u32 *deltaArray = anim->ptrDeltaArray;
	s32 bitIndex = 0;
	s32 acc[3] = {0, 0, 0};

	for (s32 k = 0; k < count; k++)
	{
		u32 c[3];

		if (deltaArray != NULL)
		{
			u32 temporal = deltaArray[k];
			s32 bits[3];
			s32 base[3];

			bits[0] = (temporal >> 6) & 7;
			bits[1] = (temporal >> 3) & 7;
			bits[2] = temporal & 7;
			base[0] = OxideFit_SignExtend(temporal >> 25, 7) << 1;
			base[1] = OxideFit_SignExtend(temporal >> 17, 8);
			base[2] = OxideFit_SignExtend(temporal >> 9, 8);

			for (s32 i = 0; i < 3; i++)
			{
				s32 value = OxideFit_ReadBits(stream, &bitIndex, bits[i] + 1);

				// an all-bits field restarts the running value; any other is a delta from it
				if (bits[i] == OXIDE_FIT_DELTA_FULL_BITS)
				{
					acc[i] = (s8)value;
				}
				else
				{
					acc[i] = (s8)(acc[i] + value + base[i]);
				}

				c[i] = (u8)acc[i];
			}
		}
		else
		{
			c[0] = stream[(k * 3) + 0];
			c[1] = stream[(k * 3) + 1];
			c[2] = stream[(k * 3) + 2];
		}

		out[k][0] = (s16)(u16)((c[0] + (u32)(frame->pos.x & OXIDE_FIT_FRAME_ORIGIN_X_MASK)) << OXIDE_FIT_GTE_VERTEX_SHIFT);
		out[k][1] = (s16)(u16)(((c[1] + (u32)(u16)frame->pos.y) << OXIDE_FIT_GTE_VERTEX_SHIFT) & OXIDE_FIT_VERTEX_Y_MASK);
		out[k][2] = (s16)(u16)((u32)(frame->pos.z + (s32)c[2]) << OXIDE_FIT_GTE_VERTEX_SHIFT);
	}
}

static b32 OxideFit_BuildPipeTable(const struct ModelHeader *mh)
{
	static const u16 pipeVerts[2][OXIDE_FIT_PIPE_VERT_COUNT] = OXIDE_FIT_PIPE_VERTS;
	static const s32 neutral[2][3] = {{OXIDE_FIT_EXHAUST_X, OXIDE_FIT_EXHAUST_Y, OXIDE_FIT_EXHAUST_Z}, {-OXIDE_FIT_EXHAUST_X, OXIDE_FIT_EXHAUST_Y, OXIDE_FIT_EXHAUST_Z}};
	s16 verts[OXIDE_FIT_PIPE_VERTS_MAX + 1][3];
	s32 vertexCount = 0;
	const u32 *commands;

	s_oxfitHeader = mh;
	s_oxfitHeaderAnims = mh->ptrAnimations;
	s_oxfitHeaderCommands = mh->ptrCommandList;
	s_oxfitUsable = 0;
	memset(s_oxfitAnimStored, 0, sizeof(s_oxfitAnimStored));

	if ((mh->ptrAnimations == NULL) || (mh->ptrCommandList == 0) || (mh->numAnimations == 0) || (mh->numAnimations > OXIDE_FIT_PIPE_MAX_ANIMS))
	{
		return 0;
	}

	// the same mesh the pipe vertex indices were taken from: one vertex per command without the cached bit
	commands = (const u32 *)(uintptr_t)mh->ptrCommandList;
	for (s32 i = 1; commands[i] != (u32)OXIDE_FIT_COMMAND_LIST_END; i++)
	{
		if (i > OXIDE_FIT_COMMAND_LIST_CAP)
		{
			return 0;
		}

		if (((commands[i] >> 16) != 0) && ((commands[i] & OXIDE_FIT_CACHED_VERTEX_BIT) == 0))
		{
			vertexCount++;
		}
	}

	if (vertexCount != OXIDE_FIT_VERTEX_COUNT)
	{
		return 0;
	}

	for (s32 a = 0; a < (s32)mh->numAnimations; a++)
	{
		const struct ModelAnim *anim = mh->ptrAnimations[a];
		s32 numFrames;
		s32 stored;

		if (anim == NULL)
		{
			continue;
		}

		numFrames = anim->numFrames & OXIDE_FIT_ANIM_FRAME_MASK;
		stored = ((anim->numFrames & OXIDE_FIT_ANIM_INTERP_BIT) != 0) ? (((numFrames - 1) >> 1) + 1) : numFrames;
		if ((stored <= 0) || (stored > OXIDE_FIT_PIPE_MAX_FRAMES))
		{
			return 0;
		}

		for (s32 f = 0; f < stored; f++)
		{
			const struct ModelFrame *frame = (const struct ModelFrame *)((const u8 *)anim + sizeof(struct ModelAnim) + ((u32)f * (u16)anim->frameSize));

			OxideFit_DecodeFrame(anim, frame, OXIDE_FIT_PIPE_VERTS_MAX + 1, verts);

			for (s32 p = 0; p < 2; p++)
			{
				for (s32 axis = 0; axis < 3; axis++)
				{
					s32 sum = 0;

					for (s32 v = 0; v < OXIDE_FIT_PIPE_VERT_COUNT; v++)
					{
						sum += verts[pipeVerts[p][v]][axis];
					}

					s_oxfitFrames[a][f].sum[p][axis] = (s16)sum;
				}
			}
		}

		s_oxfitAnimNumFrames[a] = anim->numFrames;
		s_oxfitAnimStored[a] = (u8)stored;
	}

	// the neutral pose (the drive animation's middle frame) must be where the pipes were measured; any other
	// mesh with the same vertex count is rejected here
	if (s_oxfitAnimStored[0] == 0)
	{
		return 0;
	}

	for (s32 p = 0; p < 2; p++)
	{
		for (s32 axis = 0; axis < 3; axis++)
		{
			s32 centre = s_oxfitFrames[0][s_oxfitAnimStored[0] >> 1].sum[p][axis] / OXIDE_FIT_PIPE_VERT_COUNT;
			s32 delta = centre - neutral[p][axis];

			if ((delta > OXIDE_FIT_PIPE_TOLERANCE) || (delta < -OXIDE_FIT_PIPE_TOLERANCE))
			{
				return 0;
			}
		}
	}

	s_oxfitUsable = 1;
	return 1;
}

static b32 OxideFit_EnsurePipeTable(const struct ModelHeader *mh)
{
	if ((s_oxfitHeader != mh) || (s_oxfitHeaderAnims != (const void *)mh->ptrAnimations) || (s_oxfitHeaderCommands != mh->ptrCommandList))
	{
		OxideFit_BuildPipeTable(mh);
	}

	return s_oxfitUsable;
}

static b32 OxideFit_ModelIsOxide(const struct Model *model)
{
	if (model == NULL)
	{
		return 0;
	}

	for (s32 w = 0; w < MODEL_NAME_WORD_COUNT; w++)
	{
		if (ModelName_ReadWord(model->name, w) != ModelName_ReadWord(data.MetaDataCharacters[NITROS_OXIDE].name_Debug, w))
		{
			return 0;
		}
	}

	return 1;
}

s32 OxideFit_InstanceSize(const struct Instance *inst)
{
	const struct Thread *thread;
	const struct Driver *driver;
	u32 gameMode1;

	if (inst == NULL)
	{
		return 0;
	}

	thread = inst->thread;
	if (thread == NULL)
	{
		return 0;
	}

	// only a racer, a CPU racer or a ghost carries a driver; every other instance with his model is a prop
	if ((thread->modelIndex != DYNAMIC_PLAYER) && (thread->modelIndex != DYNAMIC_ROBOT_CAR) && (thread->modelIndex != DYNAMIC_GHOST))
	{
		return 0;
	}

	if ((thread->object == NULL) || !OxideFit_ModelIsOxide(inst->model))
	{
		return 0;
	}

	driver = (const struct Driver *)thread->object;
	gameMode1 = sdata->gGT->gameMode1;

	// the menus and cutscenes draw him at the file's size (the select screen sizes its own copy)
	if ((gameMode1 & (GAME_CUTSCENE | MAIN_MENU)) != 0)
	{
		return FP_ONE;
	}

	// ghosts: slot 1 is the disc's own N. Tropy / N. Oxide ghost; slot 0 is one a player recorded
	if (thread->modelIndex == DYNAMIC_GHOST)
	{
		return (driver->ghostID == 1) ? FP_ONE : OXIDE_RACE_SCALE;
	}

	// the Adventure boss is the one CPU racer of a boss race
	if (((gameMode1 & ADVENTURE_BOSS) != 0) && (thread->modelIndex == DYNAMIC_ROBOT_CAR))
	{
		return FP_ONE;
	}

	return OXIDE_RACE_SCALE;
}

s32 OxideFit_DriverSize(const struct Driver *driver)
{
	if (driver == NULL)
	{
		return 0;
	}

	return OxideFit_InstanceSize(driver->instSelf);
}

s32 OxideFit_Grow(const struct Driver *driver, s32 value, s32 atFullSize)
{
	s32 size = OxideFit_DriverSize(driver);

	if (size == 0)
	{
		return value;
	}

	return (s32)(((s64)value * ((atFullSize * (s64)size) >> FRACTIONAL_BITS)) >> FRACTIONAL_BITS);
}

int OxideFit_PipeOffsets(const struct Instance *inst, s32 out[2][3])
{
	s32 size = OxideFit_InstanceSize(inst);
	const struct ModelHeader *mh;
	s32 sum[2][3] = {{4 * OXIDE_FIT_EXHAUST_X, 4 * OXIDE_FIT_EXHAUST_Y, 4 * OXIDE_FIT_EXHAUST_Z},
	                 {-4 * OXIDE_FIT_EXHAUST_X, 4 * OXIDE_FIT_EXHAUST_Y, 4 * OXIDE_FIT_EXHAUST_Z}};
	s32 shift = OXIDE_FIT_EMITTER_SHIFT_ONE_FRAME;
	s32 scale[3];
	s32 instScale[3];
	s32 headerScale[3];

	if (size == 0)
	{
		return 0;
	}

	if ((inst->model->headers == NULL) || (inst->model->numHeaders <= 0))
	{
		return 0;
	}

	mh = &inst->model->headers[0];

	if (OxideFit_EnsurePipeTable(mh) && (inst->animIndex < OXIDE_FIT_PIPE_MAX_ANIMS) && (s_oxfitAnimStored[inst->animIndex] != 0))
	{
		// the frame the renderer draws (RenderBucket_GetFrame); an interpolated animation counts in half frames
		s32 raw = s_oxfitAnimNumFrames[inst->animIndex];
		s32 lastFrame = (raw & OXIDE_FIT_ANIM_FRAME_MASK) - 1;
		s32 frameIndex = (u16)inst->animFrame;
		s32 hasNext = 0;
		const struct OxideFitPipeFrame *frames = s_oxfitFrames[inst->animIndex];

		if ((raw & OXIDE_FIT_ANIM_INTERP_BIT) != 0)
		{
			lastFrame >>= 1;
			hasNext = frameIndex & 1;
			frameIndex >>= 1;
		}

		if (frameIndex > lastFrame)
		{
			frameIndex = lastFrame;
		}

		for (s32 p = 0; p < 2; p++)
		{
			for (s32 axis = 0; axis < 3; axis++)
			{
				sum[p][axis] = frames[frameIndex].sum[p][axis];

				if (hasNext != 0)
				{
					sum[p][axis] += frames[(frameIndex < lastFrame) ? (frameIndex + 1) : frameIndex].sum[p][axis];
				}
			}
		}

		if (hasNext != 0)
		{
			shift = OXIDE_FIT_EMITTER_SHIFT_TWO_FRAMES;
		}
	}

	instScale[0] = inst->scale.x;
	instScale[1] = inst->scale.y;
	instScale[2] = inst->scale.z;
	headerScale[0] = mh->scale.x;
	headerScale[1] = mh->scale.y;
	headerScale[2] = mh->scale.z;

	// the renderer's per-axis scale of a vertex unit (RenderBucket_BuildM3x3), in 12-bit fixed point
	for (s32 axis = 0; axis < 3; axis++)
	{
		s32 craftScale = (instScale[axis] * size) >> FRACTIONAL_BITS;

		scale[axis] = ((headerScale[axis] >> OXIDE_FIT_HEADER_SCALE_SHIFT) * craftScale) >> FRACTIONAL_BITS;
	}

	// vertex units -> emitter units: x scale / 4096 (the matrix's fraction) x 256 (emitter units per world unit),
	// over the pipe's four vertices (and two frames when the animation is interpolated)
	for (s32 p = 0; p < 2; p++)
	{
		for (s32 axis = 0; axis < 3; axis++)
		{
			out[p][axis] = (sum[p][axis] * scale[axis]) >> shift;
		}
	}

	return 1;
}

#endif

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031b50-0x80031bdc.
void LOAD_GlobalModelPtrs_MPK()
{
	struct GameTracker *gGT = sdata->gGT;

	for (int i = 0; i < LOAD_DRIVER_MODEL_EXTRA_COUNT; i++)
	{
		struct Model *m = data.driverModelExtras[i].model;

		if (m == NULL)
		{
			continue;
		}

		if (m->id == -1)
		{
			continue;
		}

		gGT->modelPtr[m->id] = m;
	}

	if (sdata->PLYROBJECTLIST != 0)
	{
		LibraryOfModels_Store(gGT, -1, (struct Model **)sdata->PLYROBJECTLIST);
	}
}

// NOTE(aalhendi): ASM-verified NTSC-U 926 0x80031bdc-0x80031c1c.
void LOAD_HubSwapPtrs(struct GameTracker *gGT)
{
	struct Level *oldLev1;
	struct VisMem *oldVisMem1;
	struct VisMem *oldVisMem2;

	// if no secondary lev exists, quit
	if (gGT->level2 == 0)
	{
		return;
	}

	oldLev1 = gGT->level1;
	oldVisMem1 = gGT->visMem1;
	oldVisMem2 = gGT->visMem2;

	gGT->level1 = gGT->level2;
	gGT->boolHubSwapped = 1;

	gGT->level2 = oldLev1;
	gGT->visMem1 = oldVisMem2;
	gGT->visMem2 = oldVisMem1;
}
