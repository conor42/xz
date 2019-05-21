/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include "common.h"
#include "radix_internal.h"

#if defined(__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__ >= 407)
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized" /* warning: 'rpt_head_next' may be used uninitialized in this function */
#elif defined(_MSC_VER)
#  pragma warning(disable : 4701) /* warning: 'rpt_head_next' may be used uninitialized in this function */
#endif

#define MATCH_BUFFER_SHIFT 8
#define MATCH_BUFFER_ELBOW_BITS 17
#define MATCH_BUFFER_ELBOW (1UL << MATCH_BUFFER_ELBOW_BITS)
#define MIN_MATCH_BUFFER_SIZE 256U /* min buffer size at least FL2_SEARCH_DEPTH_MAX + 2 for bounded build */
#define MAX_MATCH_BUFFER_SIZE (1UL << 24) /* max buffer size constrained by 24-bit link values */

static void builder_init_tails(rmf_builder* const tbl)
{
	for (size_t i = 0; i < RADIX8_TABLE_SIZE; i += 2) {
		tbl->tails_8[i].prev_index = RADIX_NULL_LINK;
		tbl->tails_8[i + 1].prev_index = RADIX_NULL_LINK;
	}
	for (size_t i = 0; i < RADIX16_TABLE_SIZE; i += 2) {
		tbl->tails_16[i].prev_index = RADIX_NULL_LINK;
		tbl->tails_16[i + 1].prev_index = RADIX_NULL_LINK;
	}
}

static size_t calc_buf_size(size_t dictionary_size)
{
	size_t buffer_size = dictionary_size >> MATCH_BUFFER_SHIFT;
	if (buffer_size > MATCH_BUFFER_ELBOW) {
		size_t extra = 0;
		unsigned n = MATCH_BUFFER_ELBOW_BITS - 1;
		for (; (4UL << n) <= buffer_size; ++n)
			extra += MATCH_BUFFER_ELBOW >> 4;
		if ((3UL << n) <= buffer_size)
			extra += MATCH_BUFFER_ELBOW >> 5;
		buffer_size = MATCH_BUFFER_ELBOW + extra;
	}
	buffer_size = my_min(buffer_size, MAX_MATCH_BUFFER_SIZE);
	buffer_size = my_max(buffer_size, MIN_MATCH_BUFFER_SIZE);
	return buffer_size;
}

extern rmf_builder*
rmf_create_builder(rmf_match_table* const tbl, rmf_builder *builder, const lzma_allocator *allocator)
{
	size_t match_buffer_size = calc_buf_size(tbl->dictionary_size);

	if (!builder) {
		builder = lzma_alloc(
			sizeof(rmf_builder) + (match_buffer_size - 1) * sizeof(rmf_build_match), allocator);

		if (builder == NULL)
			return NULL;

		builder->table = tbl->table;
		builder->match_buffer_size = match_buffer_size;
		builder_init_tails(builder);
	}
	builder->max_len = tbl->is_struct ? STRUCTURED_MAX_LENGTH : BITPACK_MAX_LENGTH;
	builder->match_buffer_limit = match_buffer_size;

	return builder;
}

static int dict_is_struct(size_t const dictionary_size)
{
	return dictionary_size > ((size_t)1 << RADIX_LINK_BITS);
}

static size_t dict_allocation_size(size_t dictionary_size, int is_struct)
{
	return is_struct ? ((dictionary_size + 3U) / 4U) * sizeof(rmf_unit)
		: dictionary_size * sizeof(uint32_t);
}

static void init_list_heads(rmf_match_table* const tbl)
{
	for (size_t i = 0; i < RADIX16_TABLE_SIZE; i += 2) {
		tbl->list_heads[i].head = RADIX_NULL_LINK;
		tbl->list_heads[i].count = 0;
		tbl->list_heads[i + 1].head = RADIX_NULL_LINK;
		tbl->list_heads[i + 1].count = 0;
	}
	// Flag the table state as initialized by setting st_index to the end value
	tbl->st_index = 0;
	tbl->end_index = 0;
}

extern bool
rmf_options_valid(const lzma_options_lzma *const options)
{
	return options->dict_size >= DICTIONARY_SIZE_MIN
		&& options->dict_size <= DICTIONARY_SIZE_MAX
		&& (options->depth == 0 || options->depth >= DEPTH_MIN)
		&& options->depth <= DEPTH_MAX
		&& options->overlap_fraction <= OVERLAP_MAX;
}

/* rmf_create_match_table() :
 * Create a match table. Reduce the dict size to input size if possible.
 * A thread_count of 0 will be raised to 1.
 */
extern rmf_match_table*
rmf_create_match_table(const lzma_options_lzma *const options, const lzma_allocator *allocator)
{
	int const is_struct = dict_is_struct(options->dict_size);

	DEBUGLOG(3, "rmf_create_match_table : is_struct %d, dict %u", is_struct, (uint32_t)options->dict_size);

	size_t const allocation_size = dict_allocation_size(options->dict_size, is_struct);
	rmf_match_table* const tbl = lzma_alloc(sizeof(rmf_match_table) + allocation_size - sizeof(uint32_t), allocator);
	if (tbl == NULL)
		return NULL;

	tbl->allocation_size = allocation_size;
	tbl->is_struct = is_struct;
	tbl->dictionary_size = options->dict_size;
	tbl->depth = options->depth;
	tbl->divide_and_conquer = options->divide_and_conquer;
	tbl->progress = 0;

	init_list_heads(tbl);
	
	return tbl;
}

extern void
rmf_free_match_table(rmf_match_table* const tbl, const lzma_allocator *allocator)
{
	if (tbl == NULL)
		return;

	DEBUGLOG(3, "rmf_free_match_table");

	lzma_free(tbl, allocator);
}

extern uint8_t
rmf_compatible_parameters(const rmf_match_table* const tbl,
	const rmf_builder* const builder, const lzma_options_lzma *const options)
{
	return tbl->allocation_size >= dict_allocation_size(options->dict_size, dict_is_struct(options->dict_size))
		&& builder
		&& builder->match_buffer_size >= calc_buf_size(options->dict_size);
}

/* Before calling rmf_apply_parameters(), check options by calling rmf_compatible_parameters() */
extern void
rmf_apply_parameters(rmf_match_table* const tbl, const lzma_options_lzma *const options)
{
	tbl->dictionary_size = options->dict_size;
	tbl->depth = options->depth;
	tbl->divide_and_conquer = options->divide_and_conquer;
	tbl->is_struct = dict_is_struct(options->dict_size);
}

static void rmf_handle_repeat(rmf_build_match* const match_buffer,
	const uint8_t* const data_block,
	size_t const next,
	uint32_t count,
	uint32_t const rpt_len,
	uint32_t const depth,
	uint32_t const max_len)
{
	size_t pos = next;
	uint32_t length = depth + rpt_len;

	const uint8_t* const data = data_block + match_buffer[pos].from;
	const uint8_t* const data_2 = data - rpt_len;

	while (data[length] == data_2[length] && length < max_len)
		++length;

	for (; length <= max_len && count; --count) {
		size_t next_i = match_buffer[pos].next & 0xFFFFFF;
		match_buffer[pos].next = (uint32_t)next_i | (length << 24);
		length += rpt_len;
		pos = next_i;
	}
	for (; count; --count) {
		size_t next_i = match_buffer[pos].next & 0xFFFFFF;
		match_buffer[pos].next = (uint32_t)next_i | (max_len << 24);
		pos = next_i;
	}
}

typedef struct
{
	size_t pos;
	const uint8_t* data_src;
	union src_data_u src;
} brute_force_match;

static void brute_force_buffered(rmf_builder* const tbl,
	const uint8_t* const data_block,
	size_t const block_start,
	size_t pos,
	size_t const list_count,
	size_t const slot,
	size_t const depth,
	size_t const max_depth)
{
	brute_force_match buffer[MAX_BRUTE_FORCE_LIST_SIZE + 1];
	const uint8_t* const data_src = data_block + depth;
	size_t const limit = max_depth - depth;
	const uint8_t* const start = data_src + block_start;
	size_t i = 0;
	for (;;) {
		/* Load all locations from the match buffer */
		buffer[i].pos = pos;
		buffer[i].data_src = data_src + tbl->match_buffer[pos].from;
		buffer[i].src.u32 = tbl->match_buffer[pos].src.u32;

		if (++i >= list_count)
			break;

		pos = tbl->match_buffer[pos].next & 0xFFFFFF;
	}
	i = 0;
	do {
		size_t longest = 0;
		size_t j = i + 1;
		size_t longest_index = j;
		const uint8_t* const data = buffer[i].data_src;
		do {
			/* Begin with the remaining chars pulled from the match buffer */
			size_t len_test = slot;
			while (len_test < 4 && buffer[i].src.chars[len_test] == buffer[j].src.chars[len_test] && len_test - slot < limit)
				++len_test;

			len_test -= slot;
			if (len_test) {
				/* Complete the match length count in the raw input buffer */
				const uint8_t* data_2 = buffer[j].data_src;
				while (data[len_test] == data_2[len_test] && len_test < limit)
					++len_test;
			}
			if (len_test > longest) {
				longest_index = j;
				longest = len_test;
				if (len_test >= limit)
					break;
			}
		} while (++j < list_count);
		if (longest > 0) {
			/* If the existing match was extended, store the new link and length info in the match buffer */
			pos = buffer[i].pos;
			tbl->match_buffer[pos].next = (uint32_t)(buffer[longest_index].pos | ((depth + longest) << 24));
		}
		++i;
	} while (i < list_count - 1 && buffer[i].data_src >= start);
}

/* Lengthen and divide buffered chains into smaller chains, save them on a stack and process in turn. 
 * The match finder spends most of its time here.
 */
FORCE_INLINE_TEMPLATE
void recurse_list_chunk_generic(rmf_builder* const tbl,
	const uint8_t* const data_block,
	size_t const block_start,
	uint32_t depth,
	uint32_t const max_depth,
	uint32_t list_count,
	size_t const stack_base)
{
	uint32_t const base_depth = depth;
	size_t st_index = stack_base;
	size_t pos = 0;
	++depth;
	/* The last element is done separately and won't be copied back at the end */
	--list_count;
	do {
		size_t const radix_8 = tbl->match_buffer[pos].src.chars[0];
		/* Seen this char before? */
		uint32_t const prev = tbl->tails_8[radix_8].prev_index;
		tbl->tails_8[radix_8].prev_index = (uint32_t)pos;
		if (prev != RADIX_NULL_LINK) {
			++tbl->tails_8[radix_8].list_count;
			/* Link the previous occurrence to this one and record the new length */
			tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
		}
		else {
			tbl->tails_8[radix_8].list_count = 1;
			/* Add the new sub list to the stack */
			tbl->stack[st_index].head = (uint32_t)pos;
			/* This will be converted to a count at the end */
			tbl->stack[st_index].count = (uint32_t)radix_8;
			++st_index;
		}
		++pos;
	} while (pos < list_count);

	{   /* Do the last element */
		size_t const radix_8 = tbl->match_buffer[pos].src.chars[0];
		/* Nothing to do if there was no previous */
		uint32_t const prev = tbl->tails_8[radix_8].prev_index;
		if (prev != RADIX_NULL_LINK) {
			++tbl->tails_8[radix_8].list_count;
			tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
		}
	}
	/* Convert radix values on the stack to counts and reset any used tail slots */
	for (size_t j = stack_base; j < st_index; ++j) {
		tbl->tails_8[tbl->stack[j].count].prev_index = RADIX_NULL_LINK;
		tbl->stack[j].count = (uint32_t)tbl->tails_8[tbl->stack[j].count].list_count;
	}
	while (st_index > stack_base) {
		/* Pop an item off the stack */
		--st_index;
		list_count = tbl->stack[st_index].count;
		if (list_count < 2) {
			/* Nothing to match with */
			continue;
		}
		pos = tbl->stack[st_index].head;
		size_t link = tbl->match_buffer[pos].from;
		if (link < block_start) {
			/* Chain starts in the overlap region which is already encoded */
			continue;
		}
		/* Check stack space. The first comparison is unnecessary but it's a constant so should be faster */
		if (st_index > STACK_SIZE - RADIX8_TABLE_SIZE
			&& st_index > STACK_SIZE - list_count)
		{
			/* Stack may not be able to fit all possible new items. This is very rare. */
			continue;
		}
		depth = tbl->match_buffer[pos].next >> 24;
		/* Index into the 4-byte pre-loaded input char cache */
		size_t slot = (depth - base_depth) & 3;
		if (list_count <= MAX_BRUTE_FORCE_LIST_SIZE) {
			/* Quicker to use brute force, each string compared with all previous strings */
			brute_force_buffered(tbl,
				data_block,
				block_start,
				pos,
				list_count,
				slot,
				depth,
				max_depth);
			continue;
		}
		/* check for repeats at depth 4,8,16,32 etc unless depth is near max_depth */
		uint32_t const test = max_depth != 6 && ((depth & 3) == 0)
			&& (depth & (depth - 1)) == 0
			&& (max_depth >= depth + (depth >> 1));
		++depth;
		/* Create an offset data buffer pointer for reading the next bytes */
		const uint8_t* const data_src = data_block + depth;
		/* Last pass is done separately */
		if (!test && depth < max_depth) {
			size_t const prev_st_index = st_index;
			/* Last element done separately */
			--list_count;
			/* If slot is 3 then chars need to be loaded. */
			if (slot == 3 && max_depth != 6) do {
				size_t const radix_8 = tbl->match_buffer[pos].src.chars[3];
				size_t const next_index = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
				/* Pre-load the next link and data bytes. On some hardware execution can continue
				 * ahead while the data is retrieved if no operations except move are done on the data. */
				memcpy32(tbl->match_buffer[pos].src.u32, data_src + link);
				size_t const next_link = tbl->match_buffer[next_index].from;
				uint32_t const prev = tbl->tails_8[radix_8].prev_index;
				tbl->tails_8[radix_8].prev_index = (uint32_t)pos;
				if (prev != RADIX_NULL_LINK) {
					/* This char has occurred before in the chain. Link the previous (> pos) occurance with this */
					++tbl->tails_8[radix_8].list_count;
					tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
				}
				else {
					/* First occurrence in the chain */
					tbl->tails_8[radix_8].list_count = 1;
					tbl->stack[st_index].head = (uint32_t)pos;
					/* Save the char as a reference to load the count at the end */
					tbl->stack[st_index].count = (uint32_t)radix_8;
					++st_index;
				}
				pos = next_index;
				link = next_link;
			} while (--list_count != 0);
			else do {
				size_t const radix_8 = tbl->match_buffer[pos].src.chars[slot];
				size_t const next_index = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
				/* Pre-load the next link to avoid waiting for RAM access */
				size_t const next_link = tbl->match_buffer[next_index].from;
				uint32_t const prev = tbl->tails_8[radix_8].prev_index;
				tbl->tails_8[radix_8].prev_index = (uint32_t)pos;
				if (prev != RADIX_NULL_LINK) {
					++tbl->tails_8[radix_8].list_count;
					tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
				}
				else {
					tbl->tails_8[radix_8].list_count = 1;
					tbl->stack[st_index].head = (uint32_t)pos;
					tbl->stack[st_index].count = (uint32_t)radix_8;
					++st_index;
				}
				pos = next_index;
				link = next_link;
			} while (--list_count != 0);

			size_t const radix_8 = tbl->match_buffer[pos].src.chars[slot];
			uint32_t const prev = tbl->tails_8[radix_8].prev_index;
			if (prev != RADIX_NULL_LINK) {
				if (slot == 3)
					memcpy32(tbl->match_buffer[pos].src.u32, data_src + link);

				++tbl->tails_8[radix_8].list_count;
				tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
			}
			for (size_t j = prev_st_index; j < st_index; ++j) {
				tbl->tails_8[tbl->stack[j].count].prev_index = RADIX_NULL_LINK;
				tbl->stack[j].count = (uint32_t)tbl->tails_8[tbl->stack[j].count].list_count;
			}
		}
		else if (test) {
			int32_t rpt = -1;
			size_t rpt_head_next;
			uint32_t rpt_dist = 0;
			size_t const prev_st_index = st_index;
			uint32_t const rpt_depth = depth - 1;
			/* Last element done separately */
			--list_count;
			do {
				size_t const radix_8 = tbl->match_buffer[pos].src.chars[slot];
				size_t const next_index = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
				size_t const next_link = tbl->match_buffer[next_index].from;
				if ((link - next_link) > rpt_depth) {
					if (rpt > 0)
						rmf_handle_repeat(tbl->match_buffer, data_block, rpt_head_next, rpt, rpt_dist, rpt_depth, tbl->max_len);

					rpt = -1;
					uint32_t const prev = tbl->tails_8[radix_8].prev_index;
					tbl->tails_8[radix_8].prev_index = (uint32_t)pos;
					if (prev != RADIX_NULL_LINK) {
						++tbl->tails_8[radix_8].list_count;
						tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
					}
					else {
						tbl->tails_8[radix_8].list_count = 1;
						tbl->stack[st_index].head = (uint32_t)pos;
						tbl->stack[st_index].count = (uint32_t)radix_8;
						++st_index;
					}
					pos = next_index;
					link = next_link;
				}
				else {
					uint32_t const dist = (uint32_t)(link - next_link);
					if (rpt < 0 || dist != rpt_dist) {
						if (rpt > 0)
							rmf_handle_repeat(tbl->match_buffer, data_block, rpt_head_next, rpt, rpt_dist, rpt_depth, tbl->max_len);

						rpt = 0;
						rpt_head_next = next_index;
						rpt_dist = dist;
						uint32_t const prev = tbl->tails_8[radix_8].prev_index;
						tbl->tails_8[radix_8].prev_index = (uint32_t)pos;
						if (prev != RADIX_NULL_LINK) {
							++tbl->tails_8[radix_8].list_count;
							tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
						}
						else {
							tbl->tails_8[radix_8].list_count = 1;
							tbl->stack[st_index].head = (uint32_t)pos;
							tbl->stack[st_index].count = (uint32_t)radix_8;
							++st_index;
						}
					}
					else {
						++rpt;
					}
					pos = next_index;
					link = next_link;
				}
			} while (--list_count != 0);

			if (rpt > 0)
				rmf_handle_repeat(tbl->match_buffer, data_block, rpt_head_next, rpt, rpt_dist, rpt_depth, tbl->max_len);

			size_t const radix_8 = tbl->match_buffer[pos].src.chars[slot];
			uint32_t const prev = tbl->tails_8[radix_8].prev_index;
			if (prev != RADIX_NULL_LINK) {
				if (slot == 3) {
					memcpy32(tbl->match_buffer[pos].src.u32, data_src + link);
				}
				++tbl->tails_8[radix_8].list_count;
				tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
			}
			for (size_t j = prev_st_index; j < st_index; ++j) {
				tbl->tails_8[tbl->stack[j].count].prev_index = RADIX_NULL_LINK;
				tbl->stack[j].count = (uint32_t)tbl->tails_8[tbl->stack[j].count].list_count;
			}
		}
		else {
			size_t const prev_st_index = st_index;
			/* The last pass at max_depth */
			do {
				size_t const radix_8 = tbl->match_buffer[pos].src.chars[slot];
				size_t const next_index = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
				/* Pre-load the next link. */
				/* The last element in tbl->match_buffer is circular so this is never an access violation. */
				size_t const next_link = tbl->match_buffer[next_index].from;
				uint32_t const prev = tbl->tails_8[radix_8].prev_index;
				tbl->tails_8[radix_8].prev_index = (uint32_t)pos;
				if (prev != RADIX_NULL_LINK) {
					tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
				}
				else {
					tbl->stack[st_index].count = (uint32_t)radix_8;
					++st_index;
				}
				pos = next_index;
				link = next_link;
			} while (--list_count != 0);
			for (size_t j = prev_st_index; j < st_index; ++j) {
				tbl->tails_8[tbl->stack[j].count].prev_index = RADIX_NULL_LINK;
			}
			st_index = prev_st_index;
		}
	}
}

extern void
rmf_recurse_list_chunk(rmf_builder* const tbl,
	const uint8_t* const data_block,
	size_t const block_start,
	uint32_t const depth,
	uint32_t const max_depth,
	uint32_t const list_count,
	size_t const stack_base)
{
	if (list_count < 2)
		return;
	/* Template-like inline functions */
	if (list_count <= MAX_BRUTE_FORCE_LIST_SIZE)
		brute_force_buffered(tbl, data_block, block_start, 0, list_count, 0, depth, max_depth);
	else if (max_depth > 6)
		recurse_list_chunk_generic(tbl, data_block, block_start, depth, max_depth, list_count, stack_base);
	else
		recurse_list_chunk_generic(tbl, data_block, block_start, depth, 6, list_count, stack_base);
}

extern void
rmf_init_table(rmf_match_table* const tbl, const void* const data, size_t const end)
{
	DEBUGLOG(5, "rmf_init_table : size %u", (uint32_t)end);

	assert(tbl->st_index >= tbl->end_index);

	tbl->st_index = ATOMIC_INITIAL_VALUE;
	tbl->progress = 0;

	if (tbl->is_struct)
		rmf_structured_init(tbl, data, end);
	else
		rmf_bitpack_init(tbl, data, end);
}

/* Iterate the head table concurrently with other threads, and recurse each list until max_depth is reached */
extern void
rmf_build_table(rmf_match_table* const tbl,
	rmf_builder *const builder,
	int const thread,
	lzma_data_block const block)
{
	DEBUGLOG(5, "rmf_build_table : thread %u", (uint32_t)job);

	assert(block.end > block.start);

	if (tbl->is_struct)
		rmf_structured_build_table(tbl, builder, thread, block);
	else
		rmf_bitpack_build_table(tbl, builder, thread, block);

	if (thread == 0 && tbl->st_index >= RADIX_CANCEL_INDEX)
		init_list_heads(tbl);
}

/* After calling this, rmf_reset_incomplete_build() must be called when all worker threads are idle */
extern void
rmf_cancel_build(rmf_match_table * const tbl)
{
	if(tbl != NULL)
		FL2_atomic_add(tbl->st_index, RADIX_CANCEL_INDEX - ATOMIC_INITIAL_VALUE);
}

extern void
rmf_reset_incomplete_build(rmf_match_table * const tbl)
{
	if(tbl->st_index < tbl->end_index)
		init_list_heads(tbl);
}

extern int
rmf_integrity_check(const rmf_match_table* const tbl, const uint8_t* const data, size_t const pos, size_t const end, unsigned const max_depth)
{
	if (tbl->is_struct)
		return rmf_structured_integrity_check(tbl, data, pos, end, max_depth);
	else
		return rmf_bitpack_integrity_check(tbl, data, pos, end, max_depth);
}

extern void
rmf_limit_lengths(rmf_match_table* const tbl, size_t const pos)
{
	if (tbl->is_struct)
		rmf_structured_limit_lengths(tbl, pos);
	else
		rmf_bitpack_limit_lengths(tbl, pos);
}

extern uint8_t*
rmf_output_buffer(rmf_match_table* const tbl, size_t const pos)
{
	if (tbl->is_struct)
		return rmf_structured_output_buffer(tbl, pos);
	else
		return rmf_bitpack_output_buffer(tbl, pos);
}

extern size_t
rmf_memory_usage(size_t const dict_size, unsigned const thread_count)
{
	size_t size = dict_allocation_size(dict_size, dict_is_struct(dict_size));
	size_t const buf_size = calc_buf_size(dict_size);
	size += ((buf_size - 1) * sizeof(rmf_build_match) + sizeof(rmf_builder)) * thread_count;
	return size;
}
