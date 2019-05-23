///////////////////////////////////////////////////////////////////////////////
//
/// \file       radix_struct.c
/// \brief      Radix match-finder for structured table
///
//  Author:     Conor McCarthy
//
//  This source code is licensed under both the BSD-style license (found in the
//  LICENSE file in the root directory of this source tree) and the GPLv2 (found
//  in the COPYING file in the root directory of this source tree).
//  You may select, at your option, one of the above-listed licenses.
//
///////////////////////////////////////////////////////////////////////////////

#include "radix_internal.h"


#define RADIX_MAX_LENGTH STRUCTURED_MAX_LENGTH

#define init_match_link(pos, link) \
	((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK] = (uint32_t)(link)

#define get_match_link(pos) ((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK]

#define get_raw_match_link(pos) ((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK]

#define get_match_length(pos) ((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].lengths[(pos) & UNIT_MASK]

#define set_match_link(pos, link, length) \
	((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK] = (uint32_t)(link)

#define set_match_length(pos, link, length) \
	((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].lengths[(pos) & UNIT_MASK] = (uint8_t)(length)

#define set_match_link_and_length(pos, link, length) do { size_t i_ = (pos) >> UNIT_BITS, u_ = (pos) & UNIT_MASK; \
	((rmf_unit*)tbl->table)[i_].links[u_] = (uint32_t)(link); \
	((rmf_unit*)tbl->table)[i_].lengths[u_] = (uint8_t)(length); } while(0)

#define set_null(pos) ((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK] = RADIX_NULL_LINK

#define is_null(pos) (((rmf_unit*)tbl->table)[(pos) >> UNIT_BITS].links[(pos) & UNIT_MASK] == RADIX_NULL_LINK)


extern uint8_t*
rmf_structured_output_buffer(rmf_match_table* const tbl, size_t const pos)
{
	return (uint8_t*)((rmf_unit*)tbl->table + (pos >> UNIT_BITS) + ((pos & UNIT_MASK) != 0));
}


// Restrict the match lengths so that they don't reach beyond pos.
extern void
rmf_structured_limit_lengths(rmf_match_table* const tbl, size_t const pos)
{
	set_null(pos - 1);
	for (size_t length = 2; length < RADIX_MAX_LENGTH && length <= pos; ++length) {
		size_t const i = (pos - length) >> UNIT_BITS;
		size_t const u = (pos - length) & UNIT_MASK;
		if (((rmf_unit*)tbl->table)[i].links[u] != RADIX_NULL_LINK) {
			((rmf_unit*)tbl->table)[i].lengths[u] = my_min((uint8_t)length, ((rmf_unit*)tbl->table)[i].lengths[u]);
		}
	}
}


#include "radix_engine.h"