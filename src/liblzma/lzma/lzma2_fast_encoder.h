///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma2_fast_encoder.h
/// \brief      Fast LZMA2 encoder wrapper
///
//  Authors:    Conor McCarthy
//              Lasse Collin
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef LZMA_LZMA2_FAST_ENCODER_H
#define LZMA_LZMA2_FAST_ENCODER_H

#include "common.h"


extern lzma_ret lzma_flzma2_encoder_init(
		lzma_next_coder *next, const lzma_allocator *allocator,
		const lzma_filter_info *filters);

extern uint64_t lzma_flzma2_encoder_memusage(const void *options);

#endif
