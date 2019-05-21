/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#ifndef RADIX_INTERNAL_H
#define RADIX_INTERNAL_H

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
#define BITPACK_MAX_LENGTH 63U
#define STRUCTURED_MAX_LENGTH 255U
#define DEPTH_MIN 6U
#define DEPTH_MAX 254U
#define OVERLAP_MAX 14

#define RADIX_LINK_BITS 26
#define RADIX_LINK_MASK ((1U << RADIX_LINK_BITS) - 1)
#define RADIX_NULL_LINK 0xFFFFFFFFU

#define UNIT_BITS 2
#define UNIT_MASK ((1U << UNIT_BITS) - 1)

#define RADIX_CANCEL_INDEX (long)(RADIX16_TABLE_SIZE + LZMA_THREADS_MAX + 2)


typedef struct
{
	uint32_t links[1 << UNIT_BITS];
	uint8_t lengths[1 << UNIT_BITS];
} rmf_unit;


void rmf_bitpack_init(rmf_match_table* const tbl, const void* data, size_t const end);
void rmf_structured_init(rmf_match_table* const tbl, const void* data, size_t const end);
void rmf_bitpack_build_table(rmf_match_table* const tbl,
	rmf_builder* const builder,
	int const thread,
	lzma_data_block const block);
void rmf_structured_build_table(rmf_match_table* const tbl,
	rmf_builder* const builder,
	int const thread,
	lzma_data_block const block);
void rmf_recurse_list_chunk(rmf_builder* const tbl,
    const uint8_t* const data_block,
    size_t const block_start,
    uint32_t const depth,
    uint32_t const max_depth,
    uint32_t const list_count,
    size_t const stack_base);
int rmf_bitpack_integrity_check(const rmf_match_table* const tbl, const uint8_t* const data, size_t pos, size_t const end, unsigned max_depth);
int rmf_structured_integrity_check(const rmf_match_table* const tbl, const uint8_t* const data, size_t pos, size_t const end, unsigned max_depth);
void rmf_bitpack_limit_lengths(rmf_match_table* const tbl, size_t const pos);
void rmf_structured_limit_lengths(rmf_match_table* const tbl, size_t const pos);
uint8_t* rmf_bitpack_output_buffer(rmf_match_table* const tbl, size_t const pos);
uint8_t* rmf_structured_output_buffer(rmf_match_table* const tbl, size_t const pos);


#endif /* RADIX_INTERNAL_H */