///////////////////////////////////////////////////////////////////////////////
//
/// \file       radix_internal.h
/// \brief      Radix match-finder definitions and declarations
///
//  Author:     Conor McCarthy
//
//  This source code is licensed under both the BSD-style license (found in the
//  LICENSE file in the root directory of this source tree) and the GPLv2 (found
//  in the COPYING file in the root directory of this source tree).
//  You may select, at your option, one of the above-listed licenses.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef LZMA_RADIX_INTERNAL_H
#define LZMA_RADIX_INTERNAL_H

#include "common.h"
#include "radix_mf.h"
#include "lzma_encoder_private.h"


#if defined(TUKLIB_FAST_UNALIGNED_ACCESS)
#  define memcpy32(d, s) d = *(const uint32_t*)(s)
#else
#  define memcpy32(d, s) memcpy(&d, s, 4)
#endif


#define DICTIONARY_SIZE_MIN (1U << 12)
#define DICTIONARY_SIZE_MAX (UINT32_C(3) << 29)
#define MAX_REPEAT 24
#define MAX_BRUTE_FORCE_LIST_SIZE 5
#define BUFFER_LINK_MASK 0xFFFFFFU
#define MATCH_BUFFER_OVERLAP 6
#define DEPTH_MIN 6U
#define DEPTH_MAX 254U
#define OVERLAP_MAX 14

// Table building is stopped by adding this value to the stack atomic index.
#define RADIX_CANCEL_INDEX (long)(RADIX16_TABLE_SIZE + LZMA_THREADS_MAX + 2)


extern void rmf_bitpack_init(rmf_match_table* const tbl, const void* data, size_t const end);

extern void rmf_structured_init(rmf_match_table* const tbl, const void* data, size_t const end);

extern void rmf_bitpack_build_table(rmf_match_table* const tbl,
		rmf_builder* const builder,
		int const thread,
		lzma_data_block const block);

extern void rmf_structured_build_table(rmf_match_table* const tbl,
		rmf_builder* const builder,
		int const thread,
		lzma_data_block const block);

extern void rmf_recurse_list_chunk(rmf_builder* const tbl,
		const uint8_t* const data_block,
		size_t const block_start,
		uint32_t const depth,
		uint32_t const max_depth,
		uint32_t const list_count,
		size_t const stack_base);

extern void rmf_bitpack_limit_lengths(rmf_match_table* const tbl, size_t const pos);

extern void rmf_structured_limit_lengths(rmf_match_table* const tbl, size_t const pos);

extern uint8_t* rmf_bitpack_output_buffer(rmf_match_table* const tbl, size_t const pos);

extern uint8_t* rmf_structured_output_buffer(rmf_match_table* const tbl, size_t const pos);

#endif // LZMA_RADIX_INTERNAL_H