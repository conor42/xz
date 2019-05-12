/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#ifndef RADIX_MF_H
#define RADIX_MF_H


#include "atomic.h"
#include "data_block.h"

#if defined (__cplusplus)
extern "C" {
#endif

#define OVERLAP_FROM_DICT_SIZE(d, o) (((d) >> 4) * (o))

#define RMF_MIN_BYTES_PER_THREAD 1024

#define RADIX16_TABLE_SIZE ((size_t)1 << 16)
#define RADIX8_TABLE_SIZE ((size_t)1 << 8)
#define STACK_SIZE (RADIX16_TABLE_SIZE * 3)

typedef struct
{
    size_t dictionary_size;
    unsigned divide_and_conquer;
    unsigned depth;
#ifdef RMF_REFERENCE
    unsigned use_ref_mf;
#endif
} RMF_parameters;

typedef struct
{
	uint32_t head;
	uint32_t count;
} RMF_tableHead;

union src_data_u {
	uint8_t chars[4];
	uint32_t u32;
};

typedef struct
{
	uint32_t from;
	union src_data_u src;
	uint32_t next;
} RMF_buildMatch;

typedef struct
{
	uint32_t prev_index;
	uint32_t list_count;
} RMF_listTail;

typedef struct
{
	unsigned max_len;
	uint32_t* table;
	size_t match_buffer_size;
	size_t match_buffer_limit;
	RMF_listTail tails_8[RADIX8_TABLE_SIZE];
	RMF_tableHead stack[STACK_SIZE];
	RMF_listTail tails_16[RADIX16_TABLE_SIZE];
	RMF_buildMatch match_buffer[1];
} RMF_builder;

typedef struct
{
	FL2_atomic st_index;
	long end_index;
	int is_struct;
	int alloc_struct;
	size_t unreduced_dict_size;
	size_t progress;
	RMF_parameters params;
	uint32_t stack[RADIX16_TABLE_SIZE];
	RMF_tableHead list_heads[RADIX16_TABLE_SIZE];
	uint32_t table[1];
} FL2_matchTable;

typedef struct
{
	uint32_t length;
	uint32_t dist;
} RMF_match;

FL2_matchTable* RMF_createMatchTable(const RMF_parameters* const params, const lzma_allocator *allocator);
void RMF_freeMatchTable(FL2_matchTable* const tbl, const lzma_allocator *allocator);
uint8_t RMF_compatibleParameters(const FL2_matchTable* const tbl, const RMF_builder* const builder, const RMF_parameters* const params);
void RMF_applyParameters(FL2_matchTable* const tbl, const RMF_parameters* const params);
RMF_builder* RMF_createBuilder(FL2_matchTable* const tbl, RMF_builder *existing, const lzma_allocator *allocator);
void RMF_initTable(FL2_matchTable* const tbl, const void* const data, size_t const end);
int RMF_buildTable(FL2_matchTable* const tbl,
	RMF_builder* const builder,
    int const thread,
    lzma_data_block const block);
void RMF_cancelBuild(FL2_matchTable* const tbl);
void RMF_resetIncompleteBuild(FL2_matchTable* const tbl);
int RMF_integrityCheck(const FL2_matchTable* const tbl, const uint8_t* const data, size_t const pos, size_t const end, unsigned const max_depth);
void RMF_limitLengths(FL2_matchTable* const tbl, size_t const pos);
uint8_t* RMF_getTableAsOutputBuffer(FL2_matchTable* const tbl, size_t const pos);
size_t RMF_memoryUsage(size_t const dict_size, unsigned const thread_count);

#if defined (__cplusplus)
}
#endif

#endif /* RADIX_MF_H */