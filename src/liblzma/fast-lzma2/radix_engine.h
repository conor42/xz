///////////////////////////////////////////////////////////////////////////////
//
/// \file       radix_engine.h
/// \brief      Radix match-finder algorithm
///
//  Author:     Conor McCarthy
//
//  This source code is licensed under both the BSD-style license (found in the
//  LICENSE file in the root directory of this source tree) and the GPLv2 (found
//  in the COPYING file in the root directory of this source tree).
//  You may select, at your option, one of the above-listed licenses.
//
///////////////////////////////////////////////////////////////////////////////


// If a repeating byte is found, fill that section of the table with matches of distance 1.
static size_t
handle_byte_repeat(rmf_builder* const tbl, const uint8_t* const data_block,
		size_t const start, ptrdiff_t i, uint32_t depth)
{
    // Normally the last 2 bytes, but may be 4 if depth == 4 
    ptrdiff_t const last_2 = i + MAX_REPEAT / 2 - 1;

    // Find the start 
    i += (4 - (i & 3)) & 3;
    uint32_t u = *(uint32_t*)(data_block + i);
    while (i != 0 && *(uint32_t*)(data_block + i - 4) == u)
      i -= 4;
    while (i != 0 && data_block[i - 1] == (uint8_t)u)
      --i;

    ptrdiff_t const rpt_index = i;
    // No point if it's in the overlap region 
    if (last_2 >= (ptrdiff_t)start) {
        uint32_t len = depth;
        // Set matches at distance 1 and available length 
        for (i = last_2; i > rpt_index && len <= RADIX_MAX_LENGTH; --i) {
            set_match_link_and_length(i, (uint32_t)(i - 1), len);
            ++len;
        }
        // Set matches at distance 1 and max length 
        for (; i > rpt_index; --i)
            set_match_link_and_length(i, (uint32_t)(i - 1), RADIX_MAX_LENGTH);
    }
    return rpt_index;
}


// If a 2-byte repeat is found, fill that section of the table with matches of distance 2 
static size_t
handle_2byte_repeat(rmf_builder* const tbl, const uint8_t* const data_block,
		size_t const start, ptrdiff_t i, uint32_t depth)
{
    // Normally the last 2 bytes, but may be 4 if depth == 4 
    ptrdiff_t const last_2 = i + MAX_REPEAT * 2U - 4;

    // Find the start 
    ptrdiff_t realign = i & 1;
    i += (4 - (i & 3)) & 3;
    uint32_t u = *(uint32_t*)(data_block + i);
    while (i != 0 && *(uint32_t*)(data_block + i - 4) == u)
        i -= 4;
    while (i != 0 && data_block[i - 1] == data_block[i + 1])
        --i;
    i += (i & 1) ^ realign;

    ptrdiff_t const rpt_index = i;
    // No point if it's in the overlap region 
    if (i >= (ptrdiff_t)start) {
        uint32_t len = depth + (data_block[last_2 + depth] == data_block[last_2]);
        // Set matches at distance 2 and available length 
        for (i = last_2; i > rpt_index && len <= RADIX_MAX_LENGTH; i -= 2) {
            set_match_link_and_length(i, (uint32_t)(i - 2), len);
            len += 2;
        }
        // Set matches at distance 2 and max length 
        for (; i > rpt_index; i -= 2)
            set_match_link_and_length(i, (uint32_t)(i - 2), RADIX_MAX_LENGTH);
    }
    return rpt_index;
}


void
#ifdef RMF_BITPACK
rmf_bitpack_init
#else
rmf_structured_init
#endif
(rmf_match_table* const tbl, const void* const data, size_t const end)
{
    if (end <= 2) {
        for (size_t i = 0; i < end; ++i)
            set_null(i);
        tbl->end_index = 0;
        return;
    }

    set_null(0);

    const uint8_t* const data_block = (const uint8_t*)data;
    size_t st_index = 0;
    // Initial 2-byte radix value 
    size_t radix_16 = ((size_t)data_block[0] << 8) | data_block[1];
    tbl->stack[st_index++] = (uint32_t)radix_16;
    tbl->list_heads[radix_16].head = 0;
    tbl->list_heads[radix_16].count = 1;

    radix_16 = ((size_t)((uint8_t)radix_16) << 8) | data_block[2];

    ptrdiff_t i = 1;
    ptrdiff_t const block_size = end - 2;
    for (; i < block_size; ++i) {
        // Pre-load the next value for speed increase on some hardware. Execution can continue while memory read is pending 
        size_t const next_radix = ((size_t)((uint8_t)radix_16) << 8) | data_block[i + 2];

        uint32_t const prev = tbl->list_heads[radix_16].head;
        if (prev != RADIX_NULL_LINK) {
            // Link this position to the previous occurrence 
            init_match_link(i, prev);
            // Set the previous to this position 
            tbl->list_heads[radix_16].head = (uint32_t)i;
            ++tbl->list_heads[radix_16].count;
            radix_16 = next_radix;
        }
        else {
            set_null(i);
            tbl->list_heads[radix_16].head = (uint32_t)i;
            tbl->list_heads[radix_16].count = 1;
            tbl->stack[st_index++] = (uint32_t)radix_16;
            radix_16 = next_radix;
        }
    }
    // Handle the last value 
    if (tbl->list_heads[radix_16].head != RADIX_NULL_LINK)
        set_match_link_and_length(block_size, tbl->list_heads[radix_16].head, 2);
    else
        set_null(block_size);

    // Never a match at the last byte 
    set_null(end - 1);

    tbl->end_index = (uint32_t)st_index;
}


// Copy the list into a buffer and recurse it there. This decreases cache misses and allows 
// data characters to be loaded every fourth pass and stored for use in the next 4 passes 
static void
recurse_lists_buffered(rmf_builder* const tbl,
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

    // Create an offset data buffer pointer for reading the next bytes 
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
            // Pre-load next link 
            size_t const next_link = get_match_link(link);
            size_t dist = prev_link - link;
            if (dist > 2) {
                // Get 4 data characters for later. This doesn't block on a cache miss. 
                memcpy32(tbl->match_buffer[count].src.u32, data_src + link);
                // Record the actual location of this suffix 
                tbl->match_buffer[count].from = (uint32_t)link;
                // Initialize the next link 
                tbl->match_buffer[count].next = (uint32_t)(count + 1) | (depth << 24);
                rpt = 0;
                prev_link = link;
                rpt_tail = link;
                link = next_link;
            }
            else {
                rpt += 3 - dist;
                // Do the usual if the repeat is too short 
                if (rpt < MAX_REPEAT - 2) {
                    // Get 4 data characters for later. This doesn't block on a cache miss. 
					memcpy32(tbl->match_buffer[count].src.u32, data_src + link);
                    // Record the actual location of this suffix 
                    tbl->match_buffer[count].from = (uint32_t)link;
                    // Initialize the next link 
                    tbl->match_buffer[count].next = (uint32_t)(count + 1) | (depth << 24);
                    prev_link = link;
                    link = next_link;
                }
                else {
                    // Eliminate the repeat from the linked list to save time 
                    if (dist == 1) {
                        link = handle_byte_repeat(tbl, data_block, block_start, link, depth);
                        count -= MAX_REPEAT / 2;
                        orig_list_count -= (uint32_t)(rpt_tail - link);
                    }
                    else {
                        link = handle_2byte_repeat(tbl, data_block, block_start, link, depth);
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
        // Make the last element circular so pre-loading doesn't read past the end. 
        tbl->match_buffer[count - 1].next = (uint32_t)(count - 1) | (depth << 24);
        uint32_t overlap = 0;
        if (list_count < (uint32_t)(start + orig_list_count)) {
            overlap = list_count >> MATCH_BUFFER_OVERLAP;
            overlap += !overlap;
        }
        rmf_recurse_list_chunk(tbl, data_block, block_start, depth, max_depth, list_count, stack_base);
        orig_list_count -= (uint32_t)(list_count - start);
        // Copy everything back, except the last link which never changes, and any extra overlap 
        count -= overlap + (overlap == 0);
#ifdef RMF_BITPACK
        if (max_depth > RADIX_MAX_LENGTH) for (size_t pos = 0; pos < count; ++pos) {
            size_t const from = tbl->match_buffer[pos].from;
            if (from < block_start)
                return;
            uint32_t length = tbl->match_buffer[pos].next >> 24;
            length = (length > RADIX_MAX_LENGTH) ? RADIX_MAX_LENGTH : length;
            size_t const next = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
            set_match_link_and_length(from, tbl->match_buffer[next].from, length);
        }
        else
#endif
            for (size_t pos = 0; pos < count; ++pos) {
            size_t const from = tbl->match_buffer[pos].from;
            if (from < block_start)
                return;
            uint32_t const length = tbl->match_buffer[pos].next >> 24;
            size_t const next = tbl->match_buffer[pos].next & BUFFER_LINK_MASK;
            set_match_link_and_length(from, tbl->match_buffer[next].from, length);
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


// Compare each string with all others to find the best match 
static void
rmf_bruteForce(rmf_builder* const tbl,
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
    // Pre-load all locations 
    do {
        link = get_match_link(link);
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
            set_match_link_and_length(buffer[i], (uint32_t)buffer[longest_index], depth + (uint32_t)longest);

        ++i;
    // Test with block_start to avoid wasting time matching strings in the overlap region with each other 
    } while (i < list_count - 1 && buffer[i] >= block_start);
}


// Match strings at depth 2 using a 16-bit radix to lengthen to depth 4
static void
recurse_lists_16(rmf_builder* const tbl,
		const uint8_t* const data_block,
		size_t const block_start,
		size_t link,
		uint32_t count,
		uint32_t const max_depth)
{
    uint32_t const table_max_depth = my_min(max_depth, RADIX_MAX_LENGTH);
    // Offset data pointer. This function is only called at depth 2 
    const uint8_t* const data_src = data_block + 2;
    // Load radix values from the data chars 
    size_t next_radix_8 = data_src[link];
    size_t next_radix_16 = next_radix_8 + ((size_t)(data_src[link + 1]) << 8);
    size_t reset_list[RADIX8_TABLE_SIZE];
    size_t reset_count = 0;
    size_t st_index = 0;
    // Last one is done separately 
    --count;
    do
    {
        // Pre-load the next link 
        size_t const next_link = get_raw_match_link(link);
        size_t const radix_8 = next_radix_8;
        size_t const radix_16 = next_radix_16;
        // Initialization doesn't set lengths to 2 because it's a waste of time if buffering is used 
        set_match_length(link, (uint32_t)next_link, 2);

        next_radix_8 = data_src[next_link];
        next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);

        uint32_t prev = tbl->tails_8[radix_8].prev_index;
        tbl->tails_8[radix_8].prev_index = (uint32_t)link;
        if (prev != RADIX_NULL_LINK) {
            // Link the previous occurrence to this one at length 3. 
            // This will be overwritten if a 4 is found. 
            set_match_link_and_length(prev, (uint32_t)link, 3);
        }
        else {
            reset_list[reset_count++] = radix_8;
        }

        prev = tbl->tails_16[radix_16].prev_index;
        tbl->tails_16[radix_16].prev_index = (uint32_t)link;
        if (prev != RADIX_NULL_LINK) {
            ++tbl->tails_16[radix_16].list_count;
            // Link at length 4, overwriting the 3 
            set_match_link_and_length(prev, (uint32_t)link, 4);
        }
        else {
            tbl->tails_16[radix_16].list_count = 1;
            tbl->stack[st_index].head = (uint32_t)link;
            // Store a reference to this table location to retrieve the count at the end 
            tbl->stack[st_index].count = (uint32_t)radix_16;
            ++st_index;
        }
        link = next_link;
    } while (--count > 0);

    // Do the last location 
    uint32_t prev = tbl->tails_8[next_radix_8].prev_index;
    if (prev != RADIX_NULL_LINK)
        set_match_link_and_length(prev, (uint32_t)link, 3);

    prev = tbl->tails_16[next_radix_16].prev_index;
    if (prev != RADIX_NULL_LINK) {
        ++tbl->tails_16[next_radix_16].list_count;
        set_match_link_and_length(prev, (uint32_t)link, 4);
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
            // Nothing to do 
            continue;
        }
        link = tbl->stack[st_index].head;
        if (link < block_start)
            continue;
        if (st_index > STACK_SIZE - RADIX16_TABLE_SIZE
            && st_index > STACK_SIZE - list_count)
        {
            // Potential stack overflow. Rare. 
            continue;
        }
        // The current depth 
        uint32_t const depth = get_match_length(link);
        if (list_count <= MAX_BRUTE_FORCE_LIST_SIZE) {
            // Quicker to use brute force, each string compared with all previous strings 
            rmf_bruteForce(tbl, data_block,
                block_start,
                link,
                list_count,
                depth,
                table_max_depth);
            continue;
        }
        // Send to the buffer at depth 4 
        recurse_lists_buffered(tbl,
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
// Unbuffered complete processing to max_depth.
// This may be faster on CPUs without a large memory cache.
static void
recurse_lists_unbuf_16(rmf_builder* const tbl,
		const uint8_t* const data_block,
		size_t const block_start,
		size_t link,
		uint32_t count,
		uint32_t const max_depth)
{
    // Offset data pointer. This method is only called at depth 2 
    const uint8_t* data_src = data_block + 2;
    // Load radix values from the data chars 
    size_t next_radix_8 = data_src[link];
    size_t next_radix_16 = next_radix_8 + ((size_t)(data_src[link + 1]) << 8);
    rmf_list_tail* tails_8 = tbl->tails_8;
    size_t reset_list[RADIX8_TABLE_SIZE];
    size_t reset_count = 0;
    size_t st_index = 0;
    // Last one is done separately 
    --count;
    do
    {
        // Pre-load the next link 
        size_t next_link = get_raw_match_link(link);
        // Initialization doesn't set lengths to 2 because it's a waste of time if buffering is used 
        set_match_length(link, (uint32_t)next_link, 2);
        size_t radix_8 = next_radix_8;
        size_t radix_16 = next_radix_16;
        next_radix_8 = data_src[next_link];
        next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);
        uint32_t prev = tails_8[radix_8].prev_index;
        if (prev != RADIX_NULL_LINK) {
            // Link the previous occurrence to this one at length 3. 
            // This will be overwritten if a 4 is found. 
            set_match_link_and_length(prev, (uint32_t)link, 3);
        }
        else {
            reset_list[reset_count++] = radix_8;
        }
        tails_8[radix_8].prev_index = (uint32_t)link;
        prev = tbl->tails_16[radix_16].prev_index;
        if (prev != RADIX_NULL_LINK) {
            ++tbl->tails_16[radix_16].list_count;
            // Link at length 4, overwriting the 3 
            set_match_link_and_length(prev, (uint32_t)link, 4);
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
    // Do the last location 
    uint32_t prev = tails_8[next_radix_8].prev_index;
    if (prev != RADIX_NULL_LINK) {
        set_match_link_and_length(prev, (uint32_t)link, 3);
    }
    prev = tbl->tails_16[next_radix_16].prev_index;
    if (prev != RADIX_NULL_LINK) {
        ++tbl->tails_16[next_radix_16].list_count;
        set_match_link_and_length(prev, (uint32_t)link, 4);
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
            // Nothing to do 
            continue;
        }
        link = tbl->stack[st_index].head;
        if (link < block_start)
            continue;
        if (st_index > STACK_SIZE - RADIX16_TABLE_SIZE
            && st_index > STACK_SIZE - list_count)
        {
            // Potential stack overflow. Rare. 
            continue;
        }
        // The current depth 
        uint32_t depth = get_match_length(link);
        if (list_count <= MAX_BRUTE_FORCE_LIST_SIZE) {
            // Quicker to use brute force, each string compared with all previous strings 
            rmf_bruteForce(tbl, data_block,
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
        // Next depth for 1 extra char 
        ++depth;
        // and for 2 
        uint32_t depth_2 = depth + 1;
        size_t prev_st_index = st_index;
        // Last location is done separately 
        --list_count;
        // Last pass is done separately. Both of these values are always even. 
        if (depth_2 < max_depth) {
            do {
                size_t radix_8 = next_radix_8;
                size_t radix_16 = next_radix_16;
                size_t next_link = get_match_link(link);
                next_radix_8 = data_src[next_link];
                next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);
                size_t prev = tbl->tails_8[radix_8].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    // Odd numbered match length, will be overwritten if 2 chars are matched 
                    set_match_link_and_length(prev, (uint32_t)(link), depth);
                }
                else {
                    reset_list[reset_count++] = radix_8;
                }
                tbl->tails_8[radix_8].prev_index = (uint32_t)link;
                prev = tbl->tails_16[radix_16].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    ++tbl->tails_16[radix_16].list_count;
                    set_match_link_and_length(prev, (uint32_t)(link), depth_2);
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
                set_match_link_and_length(prev, (uint32_t)(link), depth);
            }
            prev = tbl->tails_16[next_radix_16].prev_index;
            if (prev != RADIX_NULL_LINK) {
                ++tbl->tails_16[next_radix_16].list_count;
                set_match_link_and_length(prev, (uint32_t)(link), depth_2);
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
                size_t next_link = get_match_link(link);
                next_radix_8 = data_src[next_link];
                next_radix_16 = next_radix_8 + ((size_t)(data_src[next_link + 1]) << 8);
                size_t prev = tbl->tails_8[radix_8].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    set_match_link_and_length(prev, (uint32_t)(link), depth);
                }
                else {
                    reset_list[reset_count++] = radix_8;
                }
                tbl->tails_8[radix_8].prev_index = (uint32_t)link;
                prev = tbl->tails_16[radix_16].prev_index;
                if (prev != RADIX_NULL_LINK) {
                    set_match_link_and_length(prev, (uint32_t)(link), depth_2);
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
                set_match_link_and_length(prev, (uint32_t)(link), depth);
            }
            prev = tbl->tails_16[next_radix_16].prev_index;
            if (prev != RADIX_NULL_LINK) {
                set_match_link_and_length(prev, (uint32_t)(link), depth_2);
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

// Atomically take a list from the head table 
static ptrdiff_t
next_list_atomic(rmf_match_table* const tbl)
{
    if (tbl->st_index < tbl->end_index) {
        long pos = lzma_atomic_increment(tbl->st_index);
        if (pos < tbl->end_index)
            return pos;
    }
    return -1;
}


// Non-atomically take a list from the head table 
static ptrdiff_t
next_list_non_atomic(rmf_match_table* const tbl)
{
    if (tbl->st_index < tbl->end_index) {
        long pos = lzma_nonatomic_increment(tbl->st_index);
        if (pos < tbl->end_index)
            return pos;
    }
    return -1;
}


// Iterate the head table concurrently with other threads, and recurse each list until max_depth is reached 
void
#ifdef RMF_BITPACK
rmf_bitpack_build_table
#else
rmf_structured_build_table
#endif
(rmf_match_table* const tbl,
		rmf_builder* const builder,
		int const thread,
		lzma_data_block const block)
{
    if (block.end == 0)
        return;

    unsigned const best = !tbl->divide_and_conquer;
    unsigned const max_depth = my_min(tbl->depth, STRUCTURED_MAX_LENGTH) & ~1;
    ptrdiff_t next_progress = (thread == 0) ? 0 : RADIX16_TABLE_SIZE;
    ptrdiff_t(*next_list_fn)(rmf_match_table* const tbl)
        = (thread >= 0) ? next_list_atomic : next_list_non_atomic;

    for (;;)
    {
        // Get the next to process 
        ptrdiff_t pos = next_list_fn(tbl);

        if (pos < 0)
            break;

        while (next_progress < pos) {
            // initial value of next_progress ensures only thread 0 executes this 
            tbl->progress += tbl->list_heads[tbl->stack[next_progress]].count;
            ++next_progress;
        }
        pos = tbl->stack[pos];
        rmf_table_head list_head = tbl->list_heads[pos];
        tbl->list_heads[pos].head = RADIX_NULL_LINK;
        if (list_head.count < 2 || list_head.head < block.start)
            continue;

        if (best && list_head.count > builder->match_buffer_limit)
        {
            // Not worth buffering or too long 
            recurse_lists_16(builder, block.data, block.start, list_head.head, list_head.count, max_depth);
        }
        else {
            recurse_lists_buffered(builder, block.data, block.start,
				list_head.head, 2, (uint8_t)max_depth, list_head.count, 0);
        }
    }
}
