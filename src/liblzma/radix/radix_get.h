///////////////////////////////////////////////////////////////////////////////
//
/// \file       radix_get.h
/// \brief      Radix match-finder table accessor
///
//  Author:     Conor McCarthy
//
//  This source code is licensed under both the BSD-style license (found in the
//  LICENSE file in the root directory of this source tree) and the GPLv2 (found
//  in the COPYING file in the root directory of this source tree).
//  You may select, at your option, one of the above-listed licenses.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef LZMA_RADIX_GET_H
#define LZMA_RADIX_GET_H


static size_t
rmf_bitpack_extend_match(const uint8_t *const data,
		const uint32_t *const table,
		ptrdiff_t const start_index,
		ptrdiff_t limit,
		uint32_t const link,
		size_t const length)
{
	ptrdiff_t end_index = start_index + length;
	ptrdiff_t const dist = start_index - link;

	if (limit > start_index + (ptrdiff_t)MATCH_LEN_MAX)
		limit = start_index + MATCH_LEN_MAX;

	while (end_index < limit && end_index - (ptrdiff_t)(table[end_index] & RADIX_LINK_MASK) == dist)
		end_index += table[end_index] >> RADIX_LINK_BITS;

	if (end_index >= limit)
		return limit - start_index;

	while (end_index < limit && data[end_index - dist] == data[end_index])
		++end_index;

	return end_index - start_index;
}


#define get_match_link(table, pos) ((const rmf_unit*)(table))[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK]

#define get_match_length(table, pos) ((const rmf_unit*)(table))[(pos) >> UNIT_BITS].lengths[(pos) & UNIT_MASK]

static size_t
rmf_structured_extend_match(const uint8_t *const data,
		const uint32_t *const table,
		ptrdiff_t const start_index,
		ptrdiff_t limit,
		uint32_t const link,
		size_t const length)
{
	ptrdiff_t end_index = start_index + length;
	ptrdiff_t const dist = start_index - link;

	if (limit > start_index + (ptrdiff_t)MATCH_LEN_MAX)
		limit = start_index + MATCH_LEN_MAX;

	while (end_index < limit && end_index - (ptrdiff_t)get_match_link(table, end_index) == dist)
		end_index += get_match_length(table, end_index);

	if (end_index >= limit)
		return limit - start_index;

	while (end_index < limit && data[end_index - dist] == data[end_index])
		++end_index;

	return end_index - start_index;
}


static force_inline_template rmf_match
rmf_get_match(lzma_data_block block,
		rmf_match_table* tbl,
		unsigned max_depth,
		int struct_tbl,
		size_t pos)
{
	if (struct_tbl)
	{
		uint32_t const link = get_match_link(tbl->table, pos);

		rmf_match match;
		match.length = 0;

		if (link == RADIX_NULL_LINK)
			return match;

		size_t const length = get_match_length(tbl->table, pos);
		size_t const dist = pos - link - 1;

		if (length == max_depth || length == STRUCTURED_MAX_LENGTH /* from repeat preprocessors */)
			match.length = (uint32_t)rmf_structured_extend_match(block.data, tbl->table, pos, block.end, link, length);
		else
			match.length = (uint32_t)length;

		match.dist = (uint32_t)dist;

		return match;
	}
	else {
		uint32_t link = tbl->table[pos];

		rmf_match match;
		match.length = 0;

		if (link == RADIX_NULL_LINK)
			return match;

		size_t const length = link >> RADIX_LINK_BITS;
		link &= RADIX_LINK_MASK;
		size_t const dist = pos - link - 1;

		if (length == max_depth || length == BITPACK_MAX_LENGTH /* from repeat preprocessors */)
			match.length = (uint32_t)rmf_bitpack_extend_match(block.data, tbl->table, pos, block.end, link, length);
		else
			match.length = (uint32_t)length;

		match.dist = (uint32_t)dist;

		return match;
	}
}


static force_inline_template rmf_match
rmf_get_next_match(lzma_data_block block,
		rmf_match_table* tbl,
		unsigned max_depth,
		int struct_tbl,
		size_t pos)
{
	if (struct_tbl)
	{
		uint32_t const link = get_match_link(tbl->table, pos);

		rmf_match match;
		match.length = 0;

		if (link == RADIX_NULL_LINK)
			return match;

		size_t const length = get_match_length(tbl->table, pos);
		size_t const dist = pos - link - 1;

		// same distance, one byte shorter 
		if (link - 1 == get_match_link(tbl->table, pos - 1))
			return match;

		if (length == max_depth || length == STRUCTURED_MAX_LENGTH /* from repeat preprocessors */)
			match.length = (uint32_t)rmf_structured_extend_match(block.data, tbl->table, pos, block.end, link, length);
		else
			match.length = (uint32_t)length;

		match.dist = (uint32_t)dist;

		return match;
	}
	else {
		uint32_t link = tbl->table[pos];

		rmf_match match;
		match.length = 0;

		if (link == RADIX_NULL_LINK)
			return match;

		size_t const length = link >> RADIX_LINK_BITS;
		link &= RADIX_LINK_MASK;
		size_t const dist = pos - link - 1;

		// same distance, one byte shorter 
		if (link - 1 == (tbl->table[pos - 1] & RADIX_LINK_MASK))
			return match;

		if (length == max_depth || length == BITPACK_MAX_LENGTH /* from repeat preprocessors */)
			match.length = (uint32_t)rmf_bitpack_extend_match(block.data, tbl->table, pos, block.end, link, length);
		else
			match.length = (uint32_t)length;

		match.dist = (uint32_t)dist;

		return match;
	}
}


#endif // LZMA_RADIX_GET_H