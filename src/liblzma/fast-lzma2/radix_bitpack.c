/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include "radix_internal.h"

#undef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))

#define RMF_BITPACK

#define RADIX_MAX_LENGTH BITPACK_MAX_LENGTH

#define InitMatchLink(pos, link) tbl->table[pos] = link

#define GetMatchLink(link) (tbl->table[link] & RADIX_LINK_MASK)

#define GetInitialMatchLink(pos) tbl->table[pos]

#define GetMatchLength(pos) (tbl->table[pos] >> RADIX_LINK_BITS)

#define SetMatchLink(pos, link, length) tbl->table[pos] = (link) | ((uint32_t)(length) << RADIX_LINK_BITS)

#define SetMatchLength(pos, link, length) tbl->table[pos] = (link) | ((uint32_t)(length) << RADIX_LINK_BITS)

#define SetMatchLinkAndLength(pos, link, length) tbl->table[pos] = (link) | ((uint32_t)(length) << RADIX_LINK_BITS)

#define SetNull(pos) tbl->table[pos] = RADIX_NULL_LINK

#define IsNull(pos) (tbl->table[pos] == RADIX_NULL_LINK)

uint8_t* RMF_bitpackAsOutputBuffer(FL2_matchTable* const tbl, size_t const pos)
{
    return (uint8_t*)(tbl->table + pos);
}

/* Restrict the match lengths so that they don't reach beyond pos */
void RMF_bitpackLimitLengths(FL2_matchTable* const tbl, size_t const pos)
{
    DEBUGLOG(5, "RMF_limitLengths : end %u, max length %u", (uint32_t)pos, RADIX_MAX_LENGTH);
    SetNull(pos - 1);
    for (uint32_t length = 2; length < RADIX_MAX_LENGTH && length <= pos; ++length) {
        uint32_t const link = tbl->table[pos - length];
        if (link != RADIX_NULL_LINK)
            tbl->table[pos - length] = (MIN(length, link >> RADIX_LINK_BITS) << RADIX_LINK_BITS) | (link & RADIX_LINK_MASK);
    }
}

#include "radix_engine.h"