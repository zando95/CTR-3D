/* String-table slots for native menu text (3DS only); the contract is in include/native_lng.h. */

#include <common.h>
#include <native_lng.h>
#include <platform/native_log.h>

#ifdef __3DS__
static char *s_nlngTable[NATIVE_LNG_END];
static char s_nlngEmpty[1];
#endif

void NativeLng_Extend(void)
{
#ifdef __3DS__
	s32 count = sdata->numLngStrings;

	if ((sdata->lngStrings == NULL) || (sdata->lngStrings == s_nlngTable))
	{
		return;
	}

	/* a file with more strings than the retail ones would overlap the native slots: keep its own table */
	if ((count < 0) || (count > NATIVE_LNG_BASE))
	{
		Platform_Log("[LNG] language file has %d strings, more than %d; native menu text is off", (int)count, (int)NATIVE_LNG_BASE);
		return;
	}

	for (s32 i = 0; i < count; i++)
	{
		s_nlngTable[i] = sdata->lngStrings[i];
	}

	for (s32 i = count; i < NATIVE_LNG_END; i++)
	{
		if ((i < NATIVE_LNG_BASE) || (s_nlngTable[i] == NULL))
		{
			s_nlngTable[i] = s_nlngEmpty;
		}
	}

	sdata->lngStrings = s_nlngTable;
	sdata->numLngStrings = NATIVE_LNG_END;
#endif
}

int NativeLng_Ready(void)
{
	return (sdata->lngStrings != NULL) && (sdata->numLngStrings >= NATIVE_LNG_END);
}
