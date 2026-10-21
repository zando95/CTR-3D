#include <common.h>

#if defined(CTR_NATIVE)
// Oxide's racer model carries the size OXIDE_RACE_SCALE is a fraction of. Each header's scale is applied to the
// vertices and bounds alike, so scaling it shrinks the whole model consistently (the retail draw reads it per
// instance). The padding halfword after the scale records the fraction already applied, so a model that is
// registered again (a hub swap) is not scaled twice.
void OxideFit_SizeRacerModel(struct Model *model)
{
#if OXIDE_RACE_SCALE != FP_ONE
	struct GameTracker *gGT = sdata->gGT;

	if ((model == NULL) || (model->headers == NULL) || ((gGT->gameMode1 & (GAME_CUTSCENE | MAIN_MENU)) != 0))
	{
		return;
	}

	for (s32 w = 0; w < MODEL_NAME_WORD_COUNT; w++)
	{
		if (ModelName_ReadWord(model->name, w) != ModelName_ReadWord(data.MetaDataCharacters[NITROS_OXIDE].name_Debug, w))
		{
			return;
		}
	}

	for (s32 i = 0; i < model->numHeaders; i++)
	{
		struct ModelHeader *header = &model->headers[i];

		if (header->_pad_scale == OXIDE_RACE_SCALE)
		{
			continue;
		}

		header->scale.x = (s16)((header->scale.x * OXIDE_RACE_SCALE) >> FRACTIONAL_BITS);
		header->scale.y = (s16)((header->scale.y * OXIDE_RACE_SCALE) >> FRACTIONAL_BITS);
		header->scale.z = (s16)((header->scale.z * OXIDE_RACE_SCALE) >> FRACTIONAL_BITS);
		header->_pad_scale = OXIDE_RACE_SCALE;
	}
#else
	(void)model;
#endif
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
