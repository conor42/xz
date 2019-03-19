///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma_encoder_presets.c
/// \brief      Encoder presets
/// \note       xz needs this even when only decoding is enabled.
//
//  Author:     Lasse Collin
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "common.h"
#include "fast-lzma2.h"


extern LZMA_API(lzma_bool)
lzma_lzma_preset(lzma_options_lzma *options, uint32_t preset)
{
	const uint32_t level = preset & LZMA_PRESET_LEVEL_MASK;
	const uint32_t flags = preset & ~LZMA_PRESET_LEVEL_MASK;
	const uint32_t supported_flags = LZMA_PRESET_EXTREME;

	if (level > 9 || (flags & ~supported_flags))
		return true;

	options->preset_dict = NULL;
	options->preset_dict_size = 0;

	options->lc = LZMA_LC_DEFAULT;
	options->lp = LZMA_LP_DEFAULT;
	options->pb = LZMA_PB_DEFAULT;

	static const uint8_t dict_pow2[]
			= { 18, 20, 21, 22, 22, 23, 23, 24, 25, 26 };
	options->dict_size = UINT32_C(1) << dict_pow2[level];

	if (level <= 3) {
		options->mode = LZMA_MODE_FAST;
		options->mf = level == 0 ? LZMA_MF_HC3 : LZMA_MF_HC4;
		options->nice_len = level <= 1 ? 128 : 273;
		static const uint8_t depths[] = { 4, 8, 24, 48 };
		options->depth = depths[level];
	} else if (!(flags & LZMA_PRESET_EXTREME)) {
		options->mf = LZMA_MF_RAD;
		FL2_compressionParameters params;
		if (FL2_isError(FL2_getLevelParameters(level, 0, &params)))
			return true;
		options->dict_size = params.dictionarySize;
//		options->overlap_fraction = params.overlapFraction;
		options->mode = params.strategy;
		options->nice_len = params.fastLength;
		options->depth = params.searchDepth;
//		options->hc3_dict_size_log = params.chainLog;
//		options->hc3_cycles = 1U << params.cyclesLog;
//		options->divide_and_conquer = params.divideAndConquer;
	} else {
		options->mode = FL2_ultra;
		options->nice_len = 273;
		options->depth = 254;
//		options->hc3_dict_size_log = FL2_CHAINLOG_MAX;
//		options->hc3_cycles = 16;
//		options->divide_and_conquer = 0;
	}

	return false;
}
