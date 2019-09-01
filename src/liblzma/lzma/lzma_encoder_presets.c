///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma_encoder_presets.c
/// \brief      Encoder presets
/// \note       xz needs this even when only decoding is enabled.
//
//  Authors:    Lasse Collin
//              Conor McCarthy
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "common.h"


static lzma_bool
lzma_lzma_preset_orig(lzma_options_lzma *options, uint32_t level, uint32_t flags)
{
	static const uint8_t dict_pow2[]
			= { 18, 20, 21, 22, 22, 23, 23, 24, 25, 26 };
	options->dict_size = UINT32_C(1) << dict_pow2[level];

	if (level <= 3) {
		options->mode = LZMA_MODE_FAST;
		options->mf = level == 0 ? LZMA_MF_HC3 : LZMA_MF_HC4;
		options->nice_len = level <= 1 ? 128 : 273;
		static const uint8_t depths[] = { 4, 8, 24, 48 };
		options->depth = depths[level];
	}
	else {
		options->mode = LZMA_MODE_NORMAL;
		options->mf = LZMA_MF_BT4;
		options->nice_len = level == 4 ? 16 : level == 5 ? 32 : 64;
		options->depth = 0;
	}

	if (flags & LZMA_PRESET_EXTREME) {
		options->mode = LZMA_MODE_NORMAL;
		options->mf = LZMA_MF_BT4;
		if (level == 3 || level == 5) {
			options->nice_len = 192;
			options->depth = 0;
		}
		else {
			options->nice_len = 273;
			options->depth = 512;
		}
	}

	// Initialize unused radix parameters to defaults
	options->near_dict_size_log = 9;
	options->near_depth = 2;
	options->divide_and_conquer = 1;

	return false;
}


extern LZMA_API(lzma_bool)
lzma_lzma_preset(lzma_options_lzma *options, uint32_t preset)
{
	const uint32_t level = preset & LZMA_PRESET_LEVEL_MASK;
	const uint32_t flags = preset & ~LZMA_PRESET_LEVEL_MASK;
	const uint32_t supported_flags = LZMA_PRESET_EXTREME | LZMA_PRESET_ORIG;

	if (level > 9 || (flags & ~supported_flags))
		return true;

	options->threads = 1;

	options->preset_dict = NULL;
	options->preset_dict_size = 0;

	options->lc = LZMA_LC_DEFAULT;
	options->lp = LZMA_LP_DEFAULT;
	options->pb = LZMA_PB_DEFAULT;

	if (level == 0 || (flags & LZMA_PRESET_ORIG))
		return lzma_lzma_preset_orig(options, level, flags);

	static const uint8_t dict_pow2[]
		= { 0, 20, 21, 21, 23, 24, 24, 25, 26, 27 };
	static const unsigned depth[]
		= { 0, 6, 14, 14, 26, 42, 42, 50, 62, 90 };
	options->dict_size = UINT32_C(1) << dict_pow2[level];
	options->depth = depth[level];
	options->mf = LZMA_MF_RAD;

	if (!(flags & LZMA_PRESET_EXTREME)) {
		options->overlap_fraction = 1 + (level >= 2);
		options->mode = LZMA_MODE_FAST + (level >= 3) + (level >= 6);
		options->nice_len = (level < 7 ) ? 32 + 8 * ((level - 1) / 2)
			: 64 + 32 * (level - 7);
		options->near_dict_size_log = (level < 5) ? 7 : level + 3;
		options->near_depth = 1 << ((level < 6) ? 0 : level - 5);
		options->divide_and_conquer = 1;
	}
	else {
		options->overlap_fraction = 4;
		options->mode = LZMA_MODE_ULTRA;
		options->nice_len = 273;
		options->depth = 254;
		options->near_dict_size_log = 14;
		options->near_depth = 16;
		options->divide_and_conquer = 0;
	}

	return false;
}
