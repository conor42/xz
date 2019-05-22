///////////////////////////////////////////////////////////////////////////////
//
/// \file       radix_mf.h
/// \brief      Radix match-finder
///
//  Author:     Conor McCarthy
//
//  This source code is licensed under both the BSD-style license (found in the
//  LICENSE file in the root directory of this source tree) and the GPLv2 (found
//  in the COPYING file in the root directory of this source tree).
//  You may select, at your option, one of the above-listed licenses.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef LZMA_RADIX_MF_H
#define LZMA_RADIX_MF_H

#include "atomic.h"
#include "data_block.h"


// 2 bytes before the end + max depth 254 + 2 bytes overrun from 32-bit load
#define MAX_READ_BEYOND_DEPTH 254

#define OVERLAP_FROM_DICT_SIZE(d, o) (((d) >> 4) * (o))

#define RMF_MIN_BYTES_PER_THREAD 1024

#define RADIX16_TABLE_SIZE ((size_t)1 << 16)
#define RADIX8_TABLE_SIZE ((size_t)1 << 8)
#define STACK_SIZE (RADIX16_TABLE_SIZE * 3)


typedef struct
{
	uint32_t head;
	uint32_t count;
} rmf_table_head;


union src_data_u {
	uint8_t chars[4];
	uint32_t u32;
};

typedef struct
{
	uint32_t from;
	union src_data_u src;
	uint32_t next;
} rmf_build_match;


typedef struct
{
	uint32_t prev_index;
	uint32_t list_count;
} rmf_list_tail;


typedef struct
{
	unsigned max_len;
	uint32_t* table;
	size_t match_buffer_size;
	size_t match_buffer_limit;
	rmf_list_tail tails_8[RADIX8_TABLE_SIZE];
	rmf_table_head stack[STACK_SIZE];
	rmf_list_tail tails_16[RADIX16_TABLE_SIZE];
	rmf_build_match match_buffer[1];
} rmf_builder;


typedef struct
{
	lzma_atomic st_index;
	long end_index;
	int is_struct;
	int divide_and_conquer;
	unsigned depth;
	size_t allocation_size;
	size_t dictionary_size;
	size_t progress;
	uint32_t stack[RADIX16_TABLE_SIZE];
	rmf_table_head list_heads[RADIX16_TABLE_SIZE];
	uint32_t table[1];
} rmf_match_table;


typedef struct
{
	uint32_t length;
	uint32_t dist;
} rmf_match;


extern bool rmf_options_valid(const lzma_options_lzma *const options);

extern rmf_match_table* rmf_create_match_table(const lzma_options_lzma *const options, const lzma_allocator *allocator);

extern void rmf_free_match_table(rmf_match_table* const tbl, const lzma_allocator *allocator);

extern uint8_t rmf_compatible_parameters(const rmf_match_table* const tbl, const rmf_builder* const builder, const lzma_options_lzma *const options);

extern void rmf_apply_parameters(rmf_match_table* const tbl, const lzma_options_lzma *const options);

extern rmf_builder* rmf_create_builder(rmf_match_table* const tbl, rmf_builder *existing, const lzma_allocator *allocator);

extern void rmf_init_table(rmf_match_table* const tbl, const void* const data, size_t const end);

extern void rmf_build_table(rmf_match_table* const tbl,
		rmf_builder* const builder,
		int const thread,
		lzma_data_block const block);

extern void rmf_cancel_build(rmf_match_table* const tbl);

extern void rmf_reset_incomplete_build(rmf_match_table* const tbl);

extern void rmf_limit_lengths(rmf_match_table* const tbl, size_t const pos);

extern uint8_t* rmf_output_buffer(rmf_match_table* const tbl, size_t const pos);

extern size_t rmf_memory_usage(size_t const dict_size, unsigned const thread_count);


#endif // LZMA_RADIX_MF_H