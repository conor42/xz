/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include <stdio.h>  

#define MAX_READ_BEYOND_DEPTH 2

/* If a repeating byte is found, fill that section of the table with matches of distance 1 */
static size_t RMF_handleRepeat(RMF_builder* const tbl, const uint8_t* const data_block, size_t const start, ptrdiff_t i, uint32_t depth)
{
    /* Normally the last 2 bytes, but may be 4 if depth == 4 */
    ptrdiff_t const last_2 = i + MAX_REPEAT / 2 - 1;

    /* Find the start */
    i += (4 - (i & 3)) & 3;
    uint32_t u = *(uint32_t*)(data_block + i);
    while (i != 0 && *(uint32_t*)(data_block + i - 4) == u)
      i -= 4;
    while (i != 0 && data_block[i - 1] == (uint8_t)u)
      --i;

    ptrdiff_t const rpt_index = i;
    /* No point if it's in the overlap region */
    if (last_2 >= (ptrdiff_t)start) {
        uint32_t len = depth;
        /* Set matches at distance 1 and available length */
        for (i = last_2; i > rpt_index && len <= RADIX_MAX_LENGTH; --i) {
            SetMatchLinkAndLength(i, (uint32_t)(i - 1), len);
            ++len;
        }
        /* Set matches at distance 1 and max length */
        for (; i > rpt_index; --i)
            SetMatchLinkAndLength(i, (uint32_t)(i - 1), RADIX_MAX_LENGTH);
    }
    return rpt_index;
}

/* If a 2-byte repeat is found, fill that section of the table with matches of distance 2 */
static size_t RMF_handleRepeat2(RMF_builder* const tbl, const uint8_t* const data_block, size_t const start, ptrdiff_t i, uint32_t depth)
{
    /* Normally the last 2 bytes, but may be 4 if depth == 4 */
    ptrdiff_t const last_2 = i + MAX_REPEAT * 2U - 4;

    /* Find the start */
    ptrdiff_t realign = i & 1;
    i += (4 - (i & 3)) & 3;
    uint32_t u = *(uint32_t*)(data_block + i);
    while (i != 0 && *(uint32_t*)(data_block + i - 4) == u)
        i -= 4;
    while (i != 0 && data_block[i - 1] == data_block[i + 1])
        --i;
    i += (i & 1) ^ realign;

    ptrdiff_t const rpt_index = i;
    /* No point if it's in the overlap region */
    if (i >= (ptrdiff_t)start) {
        uint32_t len = depth + (data_block[last_2 + depth] == data_block[last_2]);
        /* Set matches at distance 2 and available length */
        for (i = last_2; i > rpt_index && len <= RADIX_MAX_LENGTH; i -= 2) {
            SetMatchLinkAndLength(i, (uint32_t)(i - 2), len);
            len += 2;
        }
        /* Set matches at distance 2 and max length */
        for (; i > rpt_index; i -= 2)
            SetMatchLinkAndLength(i, (uint32_t)(i - 2), RADIX_MAX_LENGTH);
    }
    return rpt_index;
}

/* Initialization for the reference algortithm */
#ifdef RMF_REFERENCE
static void RMF_initReference(FL2_matchTable* const tbl, const void* const data, size_t const end)
{
    const uint8_t* const data_block = (const uint8_t*)data;
    ptrdiff_t const block_size = end - 1;
    size_t st_index = 0;
    for (ptrdiff_t i = 0; i < block_size; ++i)
    {
        size_t const radix_16 = ((size_t)data_block[i] << 8) | data_block[i + 1];
        uint32_t const prev = tbl->list_heads[radix_16].head;
        if (prev != RADIX_NULL_LINK) {
            SetMatchLinkAndLength(i, prev, 2U);
            tbl->list_heads[radix_16].head = (uint32_t)i;
            ++tbl->list_heads[radix_16].count;
        }
        else {
            SetNull(i);
            tbl->list_heads[radix_16].head = (uint32_t)i;
            tbl->list_heads[radix_16].count = 1;
            tbl->stack[st_index++] = (uint32_t)radix_16;
        }
    }
    SetNull(end - 1);
    tbl->end_index = (uint32_t)st_index;
    tbl->st_index = ATOMIC_INITIAL_VALUE;
}
#endif

void
#ifdef RMF_BITPACK
RMF_bitpackInit
#else
RMF_structuredInit
#endif
(FL2_matchTable* const tbl, const void* const data, size_t const end)
{
    if (end <= 2) {
        for (size_t i = 0; i < end; ++i)
            SetNull(i);
        tbl->end_index = 0;
        return;
    }
#ifdef RMF_REFERENCE
    if (tbl->params.use_ref_mf) {
        RMF_initReference(tbl, data, end);
        return;
    }
#endif

    SetNull(0);

    const uint8_t* const data_block = (const uint8_t*)data;
    size_t st_index = 0;
    /* Initial 2-byte radix value */
    size_t radix_16 = ((size_t)data_block[0] << 8) | data_block[1];
    tbl->stack[st_index++] = (uint32_t)radix_16;
    tbl->list_heads[radix_16].head = 0;
    tbl->list_heads[radix_16].count = 1;

    radix_16 = ((size_t)((uint8_t)radix_16) << 8) | data_block[2];

    ptrdiff_t i = 1;
    ptrdiff_t const block_size = end - 2;
    for (; i < block_size; ++i) {
        /* Pre-load the next value for speed increase on some hardware. Execution can continue while memory read is pending */
        size_t const next_radix = ((size_t)((uint8_t)radix_16) << 8) | data_block[i + 2];

        uint32_t const prev = tbl->list_heads[radix_16].head;
        if (prev != RADIX_NULL_LINK) {
            /* Link this position to the previous occurrence */
            InitMatchLink(i, prev);
            /* Set the previous to this position */
            tbl->list_heads[radix_16].head = (uint32_t)i;
            ++tbl->list_heads[radix_16].count;
            radix_16 = next_radix;
        }
        else {
            SetNull(i);
            tbl->list_heads[radix_16].head = (uint32_t)i;
            tbl->list_heads[radix_16].count = 1;
            tbl->stack[st_index++] = (uint32_t)radix_16;
            radix_16 = next_radix;
        }
    }
    /* Handle the last value */
    if (tbl->list_heads[radix_16].head != RADIX_NULL_LINK)
        SetMatchLinkAndLength(block_size, tbl->list_heads[radix_16].head, 2);
    else
        SetNull(block_size);

    /* Never a match at the last byte */
    SetNull(end - 1);

    tbl->end_index = (uint32_t)st_index;
}

/* Copy the list into a buffer and recurse it there. This decreases cache misses and allows */
/* data characters to be loaded every fourth pass and stored for use in the next 4 passes */
static void RMF_recurseListsBuffered(RMF_builder* const tbl,
    const uint8_t* const data_block,
    size_t const block_start,
    size_t link,
    uint32_t depth,
    uint32_t const max_depth,
    uint32_t orig_list_count,
    size_t const stack_base)
{
    if (orig_list_count < 2 || tbl->match_buffer_limit < 2)
        return;

    /* Create an offset data buffer pointer for reading the next bytes */
    const uint8_t* data_src = data_block + depth;
    size_t start = 0;

    do {
        uint32_t list_count = (uint32_t)(start + orig_list_count);

        if (list_count > tbl->match_buffer_limit)
            list_count = (uint32_t)tbl->match_buffer_limit;

        size_t count = start;
        size_t prev_link = (size_t)-1;
        size_t rpt = 0;
        size_t rpt_tail = link;
        for (; count < list_count; ++count) {
            /* Pre-load next link */
            size_t const next_link = GetMatchLink(link);
            size_t dist = prev_link - link;
            if (dist > 2) {
                /* Get 4 data characters for later. This doesn't block on a cache miss. */
                memcpy32(tbl->match_buffer[count].src.u32, data_src + link);
                /* Record the actual location of this suffix */
                tbl->match_buffer[count].from = (uint32_t)link;
                /* Initialize the next link */
                tbl->match_buffer[count].next = (uint32_t)(count + 1) | (depth << 24);
                rpt = 0;
                prev_link = link;
                rpt_tail = link;
                link = next_link;
            }
            else {
                rpt += 3 - dist;
                /* Do the usual if the repeat is too short */
                if (rpt < MAX_REPEAT - 2) {
                    /* Get 4 data characters for later. This doesn't block on a cache miss. */
					memcpy32(tbl->match_buffer[count].src.u32, data_src + link);
                    /* Record the actual location of this suffix */
                    tbl->match_buffer[count].from = (uint32_t)link;
                    /* Initialize the next link */
                    tbl->match_buffer[count].next = (uint32_t)(count + 1) | (depth << 24);
                    prev_link = link;
                    link = next_link;
                }
                else {
                    /* Eliminate the repeat from the linked list to save time */
                    if (dist == 1) {
                        link = RMF_handleRepeat(tbl, data_block, block_start, link, depth);
                        count -= MAX_REPEAT / 2;
                        orig_list_count -= (uint32_t)(rpt_tail - link);
                    }
                    else {
                        link = RMF_handleRepeat2(tbl, data_block, block_start, link, depth);
                        count -= MAX_REPEAT - 1;
                        orig_list_count -= (uint32_t)(rpt_tail - link) >> 1;
                    }
                    rpt = 0;
                    list_count = (uint32_t)(start + orig_list_count);

                    if (list_count > tbl->match_buffer_limit)
                        list_count = (uint32_t)tbl->match_buffer_limit;
                }
            }
        }
        count = list_count;
        /* Make the last element circular so pre-loading doesn't read past the end. */
        tbl->match_buffer[count - 1].next = (uint32_t)(count - 1) | (depth << 24);
        uint32_t overlap = 0;
        if (list_count < (uint32_t)(start + orig_list_count)) {
            overlap = list_count >> MATCH_BUFFER_OVERLAP;
            overlap += !overlap;
        }
        RMF_recurseListChunk(tbl, data_block, block_start, depth, max_depth, list_count, stack_base);
        orig_list_count -= (uint32_t)(list_count - start);
        /* Copy everything back, except the last link which never changes, and any extra overlap */
        count -= overlap + (overlap == 0);
#ifdef RMF_BITPACK
        if (max_depth > RADIX_MAX_LENGTH) for (size_t pos = 0; pos < count; ++pos) {
            size_t const from = tbl->match_buffer[pos].from;
            if (from < block_start)
                return;
            uint32_t length = tbl->match_buffer[pos].next >> 24;
            length = (length > RADIX_MAX_LENGTH) ? RADIX_MAX_LENGTH : length;
            size_t const next = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
            SetMatchLinkAndLength(from, tbl->match_buffer[next].from, length);
        }
        else
#endif
            for (size_t pos = 0; pos < count; ++pos) {
            size_t const from = tbl->match_buffer[pos].from;
            if (from < block_start)
                return;
            uint32_t const length = tbl->match_buffer[pos].next >> 24;
            size_t const next = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
            SetMatchLinkAndLength(from, tbl->match_buffer[next].from, length);
        }
        start = 0;
        if (overlap) {
            size_t dest = 0;
            for (size_t src = list_count - overlap; src < list_count; ++src) {
                tbl->match_buffer[dest].from = tbl->match_buffer[src].from;
				memcpy32(tbl->match_buffer[dest].src.u32, data_src + tbl->match_buffer[src].from);
                tbl->match_buffer[dest].next = (uint32_t)(dest + 1) | (depth << 24);
                ++dest;
            }
            start = dest;
        }
    } while (orig_list_count != 0);
}

/* Parse the list with an upper bound check on data reads. Stop at the point where bound checks are not required. */
/* Buffering is used so that parsing can continue below the bound to find a few matches without altering the main table. */
static void RMF_recurseListsBound(RMF_builder* const tbl,
    const uint8_t* const data_block,
    ptrdiff_t const block_size,
    RMF_tableHead* const list_head,
    uint32_t max_depth)
{
    uint32_t list_count = list_head->count;
    if (list_count < 2)
        return;

    ptrdiff_t link = list_head->head;
    ptrdiff_t const bounded_size = max_depth + MAX_READ_BEYOND_DEPTH;
    ptrdiff_t const bounded_start = block_size - MIN(block_size, bounded_size);
    size_t count = 0;
    size_t extra_count = (max_depth >> 4) + 4;

    list_count = MIN((uint32_t)bounded_size, list_count);
    list_count = MIN(list_count, (uint32_t)tbl->match_buffer_size);
    for (; count < list_count && extra_count; ++count) {
        ptrdiff_t next_link = GetMatchLink(link);
        if (link >= bounded_start) {
            --list_head->count;
            if (next_link < bounded_start)
                list_head->head = (uint32_t)next_link;
        }
        else {
            --extra_count;
        }
        /* Record the actual location of this suffix */
        tbl->match_buffer[count].from = (uint32_t)link;
        /* Initialize the next link */
        tbl->match_buffer[count].next = (uint32_t)(count + 1) | ((uint32_t)2 << 24);
        link = next_link;
    }
    list_count = (uint32_t)count;
    ptrdiff_t limit = block_size - 2;
    /* Create an offset data buffer pointer for reading the next bytes */
    const uint8_t* data_src = data_block + 2;
    uint32_t depth = 3;
    size_t pos = 0;
    size_t st_index = 0;
    RMF_listTail* const tails_8 = tbl->tails_8;
    do {
        link = tbl->match_buffer[pos].from;
        if (link < limit) {
            size_t const radix_8 = data_src[link];
            /* Seen this char before? */
            uint32_t const prev = tails_8[radix_8].prev_index;
            tails_8[radix_8].prev_index = (uint32_t)pos;
            if (prev != RADIX_NULL_LINK) {
                ++tails_8[radix_8].list_count;
                /* Link the previous occurrence to this one and record the new length */
                tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
            }
            else {
                tails_8[radix_8].list_count = 1;
                /* Add the new sub list to the stack */
                tbl->stack[st_index].head = (uint32_t)pos;
                /* This will be converted to a count at the end */
                tbl->stack[st_index].count = (uint32_t)radix_8;
                ++st_index;
            }
        }
        ++pos;
    } while (pos < list_count);
    /* Convert radix values on the stack to counts and reset any used tail slots */
    for (size_t j = 0; j < st_index; ++j) {
        tails_8[tbl->stack[j].count].prev_index = RADIX_NULL_LINK;
        tbl->stack[j].count = tails_8[tbl->stack[j].count].list_count;
    }
    while (st_index > 0) {
        size_t prev_st_index;

        /* Pop an item off the stack */
        --st_index;
        list_count = tbl->stack[st_index].count;
        if (list_count < 2) /* Nothing to match with */
            continue;

        pos = tbl->stack[st_index].head;
        depth = (tbl->match_buffer[pos].next >> 24);
        if (depth >= max_depth)
            continue;
        link = tbl->match_buffer[pos].from;
        if (link < bounded_start) {
            /* Chain starts before the bounded region */
            continue;
        }
        data_src = data_block + depth;
        limit = block_size - depth;
        ++depth;
        prev_st_index = st_index;
        do {
            link = tbl->match_buffer[pos].from;
            if (link < limit) {
                size_t const radix_8 = data_src[link];
                uint32_t const prev = tails_8[radix_8].prev_index;
                tails_8[radix_8].prev_index = (uint32_t)pos;
                if (prev != RADIX_NULL_LINK) {
                    ++tails_8[radix_8].list_count;
                    tbl->match_buffer[prev].next = (uint32_t)pos | (depth << 24);
                }
                else {
                    tails_8[radix_8].list_count = 1;
                    tbl->stack[st_index].head = (uint32_t)pos;
                    tbl->stack[st_index].count = (uint32_t)radix_8;
                    ++st_index;
                }
            }
            pos = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
        } while (--list_count != 0);
        for (size_t j = prev_st_index; j < st_index; ++j) {
            tails_8[tbl->stack[j].count].prev_index = RADIX_NULL_LINK;
            tbl->stack[j].count = tails_8[tbl->stack[j].count].list_count;
        }
    }
    /* Copy everything back above the bound */
    --count;
    for (pos = 0; pos < count; ++pos) {
        ptrdiff_t const from = tbl->match_buffer[pos].from;
        if (from < bounded_start)
            break;

        uint32_t length = tbl->match_buffer[pos].next >> 24;
        length = MIN(length, (uint32_t)(block_size - from));
        length = MIN(length, RADIX_MAX_LENGTH);

        size_t const next = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
        SetMatchLinkAndLength(from, tbl->match_buffer[next].from, length);
    }
}

/* Compare each string with all others to find the best match */
static void RMF_bruteForce(RMF_builder* const tbl,
    const uint8_t* const data_block,
    size_t const block_start,
    size_t link,
    size_t const list_count,
    uint32_t const depth,
    uint32_t const max_depth)
{
    const uint8_t* data_src = data_block + depth;
    size_t buffer[MAX_BRUTE_FORCE_LIST_SIZE + 1];
    size_t const limit = max_depth - depth;
    size_t i = 1;

    buffer[0] = link;
    /* Pre-load all locations */
    do {
        link = GetMatchLink(link);
        buffer[i] = link;
    } while (++i < list_count);

    i = 0;
    do {
        size_t longest = 0;
        size_t j = i + 1;
        size_t longest_index = j;
        const uint8_t* const data = data_src + buffer[i];
        do {
            const uint8_t* data_2 = data_src + buffer[j];
            size_t len_test = 0;
            while (data[len_test] == data_2[len_test] && len_test < limit)
                ++len_test;

            if (len_test > longest) {
                longest_index = j;
                longest = len_test;
                if (len_test >= limit)
                    break;
            }
        } while (++j < list_count);

        if (longest > 0)
            SetMatchLinkAndLength(buffer[i], (uint32_t)buffer[longest_index], depth + (uint32_t)longest);

        ++i;
    /* Test with block_start to avoid wasting time matching strings in the overlap region with each other */
    } while (i < list_count - 1 && buffer[i] >= block_start);
}

/* RMF_recurseLists16() : 
 * Match strings at depth 2 using a 16-bit radix to lengthen to depth 4
 */
static void RMF_recurseLists16(RMF_builder* const tbl,
    const uint8_t* const data_block,
    size_t const block_start,
    size_t link,
    uint32_t count,
    uint32_t const max_depth)
{
    uint32_t const table_max_depth = MIN(max_depth, RADIX_MAX_LENGTH);
    /* Offset data pointer. This function is only called at depth 2 */
    const uint8_t* const data_src = data_block + 2;
    /* Load radix values from the data chars */
    size_t next_radix_8 = data_src[link];
    size_t next_radix_16 = next_radix_8 + ((size_t)(data_src[link + 1]) << 8);
    size_t reset_list[RADIX8_TABLE_SIZE];
    size_t reset_count = 0;
    size_t st_index = 0;
    /* Last one is done separately */
    --count;
    do
    {
        /* Pre-load the next link */
        size_t const next_link = GetInitialMatchLink(link);
        size_t const radix_8 = next_radix_8;
        size_t const radix_16 = next_radix_16;
        /* Initialization doesn't set lengths to 2 because it's a waste of time if buffering is used */
        SetMatchLength(link, (uint32_t)next_link, 2);

        next_radix_8 = data_src[next_link];
        next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);

        uint32_t prev = tbl->tails_8[radix_8].prev_index;
        tbl->tails_8[radix_8].prev_index = (uint32_t)link;
        if (prev != RADIX_NULL_LINK) {
            /* Link the previous occurrence to this one at length 3. */
            /* This will be overwritten if a 4 is found. */
            SetMatchLinkAndLength(prev, (uint32_t)link, 3);
        }
        else {
            reset_list[reset_count++] = radix_8;
        }

        prev = tbl->tails_16[radix_16].prev_index;
        tbl->tails_16[radix_16].prev_index = (uint32_t)link;
        if (prev != RADIX_NULL_LINK) {
            ++tbl->tails_16[radix_16].list_count;
            /* Link at length 4, overwriting the 3 */
            SetMatchLinkAndLength(prev, (uint32_t)link, 4);
        }
        else {
            tbl->tails_16[radix_16].list_count = 1;
            tbl->stack[st_index].head = (uint32_t)link;
            /* Store a reference to this table location to retrieve the count at the end */
            tbl->stack[st_index].count = (uint32_t)radix_16;
            ++st_index;
        }
        link = next_link;
    } while (--count > 0);

    /* Do the last location */
    uint32_t prev = tbl->tails_8[next_radix_8].prev_index;
    if (prev != RADIX_NULL_LINK)
        SetMatchLinkAndLength(prev, (uint32_t)link, 3);

    prev = tbl->tails_16[next_radix_16].prev_index;
    if (prev != RADIX_NULL_LINK) {
        ++tbl->tails_16[next_radix_16].list_count;
        SetMatchLinkAndLength(prev, (uint32_t)link, 4);
    }

    for (size_t i = 0; i < reset_count; ++i)
        tbl->tails_8[reset_list[i]].prev_index = RADIX_NULL_LINK;

    for (size_t i = 0; i < st_index; ++i) {
        tbl->tails_16[tbl->stack[i].count].prev_index = RADIX_NULL_LINK;
        tbl->stack[i].count = tbl->tails_16[tbl->stack[i].count].list_count;
    }

    while (st_index > 0) {
        --st_index;
        uint32_t const list_count = tbl->stack[st_index].count;
        if (list_count < 2) {
            /* Nothing to do */
            continue;
        }
        link = tbl->stack[st_index].head;
        if (link < block_start)
            continue;
        if (st_index > STACK_SIZE - RADIX16_TABLE_SIZE
            && st_index > STACK_SIZE - list_count)
        {
            /* Potential stack overflow. Rare. */
            continue;
        }
        /* The current depth */
        uint32_t const depth = GetMatchLength(link);
        if (list_count <= MAX_BRUTE_FORCE_LIST_SIZE) {
            /* Quicker to use brute force, each string compared with all previous strings */
            RMF_bruteForce(tbl, data_block,
                block_start,
                link,
                list_count,
                depth,
                table_max_depth);
            continue;
        }
        /* Send to the buffer at depth 4 */
        RMF_recurseListsBuffered(tbl,
            data_block,
            block_start,
            link,
            (uint8_t)depth,
            (uint8_t)max_depth,
            list_count,
            st_index);
    }
}

#if 0
/* Unbuffered complete processing to max_depth.
 * This may be faster on CPUs without a large memory cache.
 */
static void RMF_recurseListsUnbuf16(RMF_builder* const tbl,
    const uint8_t* const data_block,
    size_t const block_start,
    size_t link,
    uint32_t count,
    uint32_t const max_depth)
{
    /* Offset data pointer. This method is only called at depth 2 */
    const uint8_t* data_src = data_block + 2;
    /* Load radix values from the data chars */
    size_t next_radix_8 = data_src[link];
    size_t next_radix_16 = next_radix_8 + ((size_t)(data_src[link + 1]) << 8);
    RMF_listTail* tails_8 = tbl->tails_8;
    size_t reset_list[RADIX8_TABLE_SIZE];
    size_t reset_count = 0;
    size_t st_index = 0;
    /* Last one is done separately */
    --count;
    do
    {
        /* Pre-load the next link */
        size_t next_link = GetInitialMatchLink(link);
        /* Initialization doesn't set lengths to 2 because it's a waste of time if buffering is used */
        SetMatchLength(link, (uint32_t)next_link, 2);
        size_t radix_8 = next_radix_8;
        size_t radix_16 = next_radix_16;
        next_radix_8 = data_src[next_link];
        next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);
        uint32_t prev = tails_8[radix_8].prev_index;
        if (prev != RADIX_NULL_LINK) {
            /* Link the previous occurrence to this one at length 3. */
            /* This will be overwritten if a 4 is found. */
            SetMatchLinkAndLength(prev, (uint32_t)link, 3);
        }
        else {
            reset_list[reset_count++] = radix_8;
        }
        tails_8[radix_8].prev_index = (uint32_t)link;
        prev = tbl->tails_16[radix_16].prev_index;
        if (prev != RADIX_NULL_LINK) {
            ++tbl->tails_16[radix_16].list_count;
            /* Link at length 4, overwriting the 3 */
            SetMatchLinkAndLength(prev, (uint32_t)link, 4);
        }
        else {
            tbl->tails_16[radix_16].list_count = 1;
            tbl->stack[st_index].head = (uint32_t)link;
            tbl->stack[st_index].count = (uint32_t)radix_16;
            ++st_index;
        }
        tbl->tails_16[radix_16].prev_index = (uint32_t)link;
        link = next_link;
    } while (--count > 0);
    /* Do the last location */
    uint32_t prev = tails_8[next_radix_8].prev_index;
    if (prev != RADIX_NULL_LINK) {
        SetMatchLinkAndLength(prev, (uint32_t)link, 3);
    }
    prev = tbl->tails_16[next_radix_16].prev_index;
    if (prev != RADIX_NULL_LINK) {
        ++tbl->tails_16[next_radix_16].list_count;
        SetMatchLinkAndLength(prev, (uint32_t)link, 4);
    }
    for (size_t i = 0; i < reset_count; ++i) {
        tails_8[reset_list[i]].prev_index = RADIX_NULL_LINK;
    }
    reset_count = 0;
    for (size_t i = 0; i < st_index; ++i) {
        tbl->tails_16[tbl->stack[i].count].prev_index = RADIX_NULL_LINK;
        tbl->stack[i].count = tbl->tails_16[tbl->stack[i].count].list_count;
    }
    while (st_index > 0) {
        --st_index;
        uint32_t list_count = tbl->stack[st_index].count;
        if (list_count < 2) {
            /* Nothing to do */
            continue;
        }
        link = tbl->stack[st_index].head;
        if (link < block_start)
            continue;
        if (st_index > STACK_SIZE - RADIX16_TABLE_SIZE
            && st_index > STACK_SIZE - list_count)
        {
            /* Potential stack overflow. Rare. */
            continue;
        }
        /* The current depth */
        uint32_t depth = GetMatchLength(link);
        if (list_count <= MAX_BRUTE_FORCE_LIST_SIZE) {
            /* Quicker to use brute force, each string compared with all previous strings */
            RMF_bruteForce(tbl, data_block,
                block_start,
                link,
                list_count,
                depth,
                max_depth);
            continue;
        }
        const uint8_t* data_src = data_block + depth;
        size_t next_radix_8 = data_src[link];
        size_t next_radix_16 = next_radix_8 + ((size_t)(data_src[link + 1]) << 8);
        /* Next depth for 1 extra char */
        ++depth;
        /* and for 2 */
        uint32_t depth_2 = depth + 1;
        size_t prev_st_index = st_index;
        /* Last location is done separately */
        --list_count;
        /* Last pass is done separately. Both of these values are always even. */
        if (depth_2 < max_depth) {
            do {
                size_t radix_8 = next_radix_8;
                size_t radix_16 = next_radix_16;
                size_t next_link = GetMatchLink(link);
                next_radix_8 = data_src[next_link];
                next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);
                size_t prev = tbl->tails_8[radix_8].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    /* Odd numbered match length, will be overwritten if 2 chars are matched */
                    SetMatchLinkAndLength(prev, (uint32_t)(link), depth);
                }
                else {
                    reset_list[reset_count++] = radix_8;
                }
                tbl->tails_8[radix_8].prev_index = (uint32_t)link;
                prev = tbl->tails_16[radix_16].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    ++tbl->tails_16[radix_16].list_count;
                    SetMatchLinkAndLength(prev, (uint32_t)(link), depth_2);
                }
                else {
                    tbl->tails_16[radix_16].list_count = 1;
                    tbl->stack[st_index].head = (uint32_t)(link);
                    tbl->stack[st_index].count = (uint32_t)(radix_16);
                    ++st_index;
                }
                tbl->tails_16[radix_16].prev_index = (uint32_t)(link);
                link = next_link;
            } while (--list_count != 0);
            size_t prev = tbl->tails_8[next_radix_8].prev_index;
            if (prev != RADIX_NULL_LINK) {
                SetMatchLinkAndLength(prev, (uint32_t)(link), depth);
            }
            prev = tbl->tails_16[next_radix_16].prev_index;
            if (prev != RADIX_NULL_LINK) {
                ++tbl->tails_16[next_radix_16].list_count;
                SetMatchLinkAndLength(prev, (uint32_t)(link), depth_2);
            }
            for (size_t i = prev_st_index; i < st_index; ++i) {
                tbl->tails_16[tbl->stack[i].count].prev_index = RADIX_NULL_LINK;
                tbl->stack[i].count = tbl->tails_16[tbl->stack[i].count].list_count;
            }
            for (size_t i = 0; i < reset_count; ++i) {
                tails_8[reset_list[i]].prev_index = RADIX_NULL_LINK;
            }
            reset_count = 0;
        }
        else {
            do {
                size_t radix_8 = next_radix_8;
                size_t radix_16 = next_radix_16;
                size_t next_link = GetMatchLink(link);
                next_radix_8 = data_src[next_link];
                next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);
                size_t prev = tbl->tails_8[radix_8].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    SetMatchLinkAndLength(prev, (uint32_t)(link), depth);
                }
                else {
                    reset_list[reset_count++] = radix_8;
                }
                tbl->tails_8[radix_8].prev_index = (uint32_t)link;
                prev = tbl->tails_16[radix_16].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    SetMatchLinkAndLength(prev, (uint32_t)(link), depth_2);
                }
                else {
                    tbl->stack[st_index].count = (uint32_t)radix_16;
                    ++st_index;
                }
                tbl->tails_16[radix_16].prev_index = (uint32_t)(link);
                link = next_link;
            } while (--list_count != 0);
            size_t prev = tbl->tails_8[next_radix_8].prev_index;
            if (prev != RADIX_NULL_LINK) {
                SetMatchLinkAndLength(prev, (uint32_t)(link), depth);
            }
            prev = tbl->tails_16[next_radix_16].prev_index;
            if (prev != RADIX_NULL_LINK) {
                SetMatchLinkAndLength(prev, (uint32_t)(link), depth_2);
            }
            for (size_t i = prev_st_index; i < st_index; ++i) {
                tbl->tails_16[tbl->stack[i].count].prev_index = RADIX_NULL_LINK;
            }
            st_index = prev_st_index;
            for (size_t i = 0; i < reset_count; ++i) {
                tails_8[reset_list[i]].prev_index = RADIX_NULL_LINK;
            }
            reset_count = 0;
        }
    }
}
#endif

#ifdef RMF_REFERENCE

/* Simple, slow, complete parsing for reference */
static void RMF_recurseListsReference(RMF_builder* const tbl,
    const uint8_t* const data_block,
    size_t const block_size,
    size_t link,
    uint32_t count,
    uint32_t const max_depth)
{
    /* Offset data pointer. This method is only called at depth 2 */
    const uint8_t* data_src = data_block + 2;
    size_t limit = block_size - 2;
    size_t st_index = 0;

    do
    {
        if (link < limit) {
            size_t const radix_8 = data_src[link];
            size_t const prev = tbl->tails_8[radix_8].prev_index;
            if (prev != RADIX_NULL_LINK) {
                ++tbl->tails_8[radix_8].list_count;
                SetMatchLinkAndLength(prev, (uint32_t)link, 3);
            }
            else {
                tbl->tails_8[radix_8].list_count = 1;
                tbl->stack[st_index].head = (uint32_t)link;
                tbl->stack[st_index].count = (uint32_t)radix_8;
                ++st_index;
            }
            tbl->tails_8[radix_8].prev_index = (uint32_t)link;
        }
        link = GetMatchLink(link);
    } while (--count > 0);
    for (size_t i = 0; i < st_index; ++i) {
        tbl->stack[i].count = tbl->tails_8[tbl->stack[i].count].list_count;
    }
    memset(tbl->tails_8, 0xFF, sizeof(tbl->tails_8));
    while (st_index > 0) {
        --st_index;
        uint32_t list_count = tbl->stack[st_index].count;
        if (list_count < 2) {
            /* Nothing to do */
            continue;
        }
        if (st_index > STACK_SIZE - RADIX8_TABLE_SIZE
            && st_index > STACK_SIZE - list_count)
        {
            /* Potential stack overflow. Rare. */
            continue;
        }
        link = tbl->stack[st_index].head;
        /* The current depth */
        uint32_t depth = GetMatchLength(link);
        if (depth >= max_depth)
            continue;
        data_src = data_block + depth;
        limit = block_size - depth;
        /* Next depth for 1 extra char */
        ++depth;
        size_t prev_st_index = st_index;
        do {
            if (link < limit) {
                size_t const radix_8 = data_src[link];
                size_t const prev = tbl->tails_8[radix_8].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    ++tbl->tails_8[radix_8].list_count;
                    SetMatchLinkAndLength(prev, (uint32_t)link, depth);
                }
                else {
                    tbl->tails_8[radix_8].list_count = 1;
                    tbl->stack[st_index].head = (uint32_t)link;
                    tbl->stack[st_index].count = (uint32_t)radix_8;
                    ++st_index;
                }
                tbl->tails_8[radix_8].prev_index = (uint32_t)link;
            }
            link = GetMatchLink(link);
        } while (--list_count != 0);
        for (size_t i = prev_st_index; i < st_index; ++i) {
            tbl->stack[i].count = tbl->tails_8[tbl->stack[i].count].list_count;
        }
        memset(tbl->tails_8, 0xFF, sizeof(tbl->tails_8));
    }
}

#endif /* RMF_REFERENCE */

/* Atomically take a list from the head table */
static ptrdiff_t RMF_getNextList_mt(FL2_matchTable* const tbl)
{
    if (tbl->st_index < tbl->end_index) {
        long pos = FL2_atomic_increment(tbl->st_index);
        if (pos < tbl->end_index)
            return pos;
    }
    return -1;
}

/* Non-atomically take a list from the head table */
static ptrdiff_t RMF_getNextList_st(FL2_matchTable* const tbl)
{
    if (tbl->st_index < tbl->end_index) {
        long pos = FL2_nonAtomic_increment(tbl->st_index);
        if (pos < tbl->end_index)
            return pos;
    }
    return -1;
}

/* Iterate the head table concurrently with other threads, and recurse each list until max_depth is reached */
void
#ifdef RMF_BITPACK
RMF_bitpackBuildTable
#else
RMF_structuredBuildTable
#endif
(FL2_matchTable* const tbl,
    size_t const job,
    unsigned const multi_thread,
    lzma_data_block const block)
{
    if (block.end == 0)
        return;

    unsigned const best = !tbl->params.divide_and_conquer;
    unsigned const max_depth = MIN(tbl->params.depth, STRUCTURED_MAX_LENGTH) & ~1;
    size_t bounded_start = max_depth + MAX_READ_BEYOND_DEPTH;
    bounded_start = block.end - MIN(block.end, bounded_start);
    ptrdiff_t next_progress = (job == 0) ? 0 : RADIX16_TABLE_SIZE;
    ptrdiff_t(*getNextList)(FL2_matchTable* const tbl)
        = multi_thread ? RMF_getNextList_mt : RMF_getNextList_st;

    for (;;)
    {
        /* Get the next to process */
        ptrdiff_t pos = getNextList(tbl);

        if (pos < 0)
            break;

        while (next_progress < pos) {
            /* initial value of next_progress ensures only thread 0 executes this */
            tbl->progress += tbl->list_heads[tbl->stack[next_progress]].count;
            ++next_progress;
        }
        pos = tbl->stack[pos];
        RMF_tableHead list_head = tbl->list_heads[pos];
        tbl->list_heads[pos].head = RADIX_NULL_LINK;
        if (list_head.count < 2 || list_head.head < block.start)
            continue;

#ifdef RMF_REFERENCE
        if (tbl->params.use_ref_mf) {
            RMF_recurseListsReference(tbl->builders[job], block.data, block.end, list_head.head, list_head.count, max_depth);
            continue;
        }
#endif
        if (list_head.head >= bounded_start) {
            RMF_recurseListsBound(tbl->builders[job], block.data, block.end, &list_head, max_depth);
            if (list_head.count < 2 || list_head.head < block.start)
                continue;
        }
        if (best && list_head.count > tbl->builders[job]->match_buffer_limit)
        {
            /* Not worth buffering or too long */
            RMF_recurseLists16(tbl->builders[job], block.data, block.start, list_head.head, list_head.count, max_depth);
        }
        else {
            RMF_recurseListsBuffered(tbl->builders[job], block.data, block.start, list_head.head, 2, (uint8_t)max_depth, list_head.count, 0);
        }
    }
}

int
#ifdef RMF_BITPACK
RMF_bitpackIntegrityCheck
#else
RMF_structuredIntegrityCheck
#endif
(const FL2_matchTable* const tbl, const uint8_t* const data, size_t pos, size_t const end, unsigned max_depth)
{
    max_depth &= ~1;
    int err = 0;
    for (pos += !pos; pos < end; ++pos) {
        if (IsNull(pos))
            continue;
        uint32_t const link = GetMatchLink(pos);
        if (link >= pos) {
            printf("Forward link at %X to %u\r\n", (uint32_t)pos, link);
            err = 1;
            continue;
        }
        uint32_t const length = GetMatchLength(pos);
        if (pos && length < RADIX_MAX_LENGTH && link - 1 == GetMatchLink(pos - 1) && length + 1 == GetMatchLength(pos - 1))
            continue;
        uint32_t len_test = 0;
        uint32_t const limit = MIN((uint32_t)(end - pos), RADIX_MAX_LENGTH);
        for (; len_test < limit && data[link + len_test] == data[pos + len_test]; ++len_test) {
        }
        if (len_test < length) {
            printf("Failed integrity check: pos %X, length %u, actual %u\r\n", (uint32_t)pos, length, len_test);
            err = 1;
        }
        if (length < max_depth && len_test > length)
            /* These occur occasionally due to splitting of chains in the buffer when long repeats are present */
            printf("Shortened match at %X: %u of %u\r\n", (uint32_t)pos, length, len_test);
    }
    return err;
}
