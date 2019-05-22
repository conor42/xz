///////////////////////////////////////////////////////////////////////////////
//
/// \file       radix_bitpack.h
/// \brief      Radix match-finder for uint32_t table
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


#define RMF_BITPACK

#define RADIX_MAX_LENGTH BITPACK_MAX_LENGTH

#define init_match_link(pos, link) tbl->table[pos] = link

#define get_match_link(link) (tbl->table[link] & RADIX_LINK_MASK)

#define get_raw_match_link(pos) tbl->table[pos]

#define get_match_length(pos) (tbl->table[pos] >> RADIX_LINK_BITS)

#define set_match_length(pos, link, length) tbl->table[pos] = (link) | ((uint32_t)(length) << RADIX_LINK_BITS)

#define set_match_link_and_length(pos, link, length) \
	tbl->table[pos] = (link) | ((uint32_t)(length) << RADIX_LINK_BITS)

#define set_null(pos) tbl->table[pos] = RADIX_NULL_LINK

#define is_null(pos) (tbl->table[pos] == RADIX_NULL_LINK)


extern uint8_t*
rmf_bitpack_output_buffer(rmf_match_table* const tbl, size_t const pos)
{
    return (uint8_t*)(tbl->table + pos);
}


// Restrict the match lengths so that they don't reach beyond pos.
extern void
rmf_bitpack_limit_lengths(rmf_match_table* const tbl, size_t const pos)
{
    set_null(pos - 1);
    for (uint32_t length = 2; length < RADIX_MAX_LENGTH && length <= pos; ++length) {
        uint32_t const link = tbl->table[pos - length];
        if (link != RADIX_NULL_LINK)
            tbl->table[pos - length] = (my_min(length, link >> RADIX_LINK_BITS) << RADIX_LINK_BITS) | (link & RADIX_LINK_MASK);
    }
}


#include "radix_engine.h"