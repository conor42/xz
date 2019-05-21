///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma2_encoder_rmf.c
/// \brief      LZMA2 encoder for radix match-finder
///
//  Authors:    Igor Pavlov
//              Conor McCarthy
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "common.h"

#include <stdlib.h>

#include "flzma.h"
#include "lzma_common.h"
#include "lzma2_enc.h"
#include "radix_mf.h"
#include "radix_internal.h"
#include "radix_get.h"
#include "range_enc.h"
#include "fastpos.h"

#define CHUNK_UNCOMPRESSED_MAX (1UL << 21U)

#define MATCH_MAX_OUT_SIZE 20

#define CHUNK_COMPRESSED_MAX (1UL << 16U)
// Need to leave enough space for expanded output from a full opt buffer with bad starting probs
#define CHUNK_SIZE (CHUNK_COMPRESSED_MAX - 2048U)
#define SQRT_CHUNK_SIZE 252U

#define CHUNK_HEADER_SIZE 5U
#define CHUNK_RESET_SHIFT 5U
#define CHUNK_UNCOMP_DICT_RESET 1U
#define CHUNK_UNCOMPRESSED 2U
#define CHUNK_COMPRESSED_FLAG 0x80U
#define CHUNK_NOTHING_RESET 0U
#define CHUNK_STATE_RESET (1U << CHUNK_RESET_SHIFT)
#define CHUNK_STATE_PROP_RESET (2U << CHUNK_RESET_SHIFT)
#define CHUNK_ALL_RESET (3U << CHUNK_RESET_SHIFT)

#define TEST_MIN_CHUNK_SIZE 0x4000U
#define RANDOM_FILTER_MARGIN_BITS 8U

#define STATE_LIT_AFTER_MATCH 4
#define STATE_LIT_AFTER_REP   5
#define STATE_MATCH_AFTER_LIT 7
#define STATE_REP_AFTER_LIT   8

#define NULL_DIST UINT32_MAX
#define mark_literal(node) (node).dist = NULL_DIST; (node).extra = 0;
#define mark_short_rep(node) (node).dist = 0; (node).extra = 0;

static const uint8_t lit_next_tbl[STATES] = { 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5 };
#define literal_next_state(s) lit_next_tbl[s]
static const uint8_t match_next_tbl[STATES] = { 7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10 };
#define match_next_state(s) match_next_tbl[s]
static const uint8_t rep_next_tbl[STATES] = { 8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11 };
#define rep_next_state(s) rep_next_tbl[s]
static const uint8_t short_rep_next_tbl[STATES] = { 9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11 };
#define short_rep_next_state(s) short_rep_next_tbl[s]

void lzma2_rmf_enc_construct(lzma2_rmf_encoder *const enc)
{
    DEBUGLOG(3, "lzma2_rmf_enc_construct");

    enc->lc = 3;
    enc->lp = 0;
    enc->pb = 2;
    enc->fast_length = 48;
    enc->len_end_max = OPT_BUF_SIZE - 1;
    enc->match_cycles = 1;
    enc->strategy = LZMA_MODE_ULTRA;
    enc->match_price_count = 0;
    enc->rep_len_price_count = 0;
    enc->dist_price_table_size = 0;
    enc->hash_buf = NULL;
    enc->hash_dict_3 = 0;
    enc->chain_mask_3 = 0;
    enc->hash_alloc_3 = 0;
}

void lzma2_rmf_enc_free(lzma2_rmf_encoder *const enc)
{
    free(enc->hash_buf);
}

#define literal_prob_tbl(enc, pos, prev_symbol) (enc->states.literal_probs + \
	(size_t)3 * (((((pos) << 8) + (prev_symbol)) & enc->lit_pos_mask) << enc->lc))

#define len_to_dist_state(len) (((len) < DIST_STATES + 1) ? (len) - 2 : DIST_STATES - 1)

#define is_lit_state(state) ((state) < 7)

HINT_INLINE
unsigned lzma_rep_1_price(lzma2_rmf_encoder* const enc, size_t const state, size_t const pos_state)
{
    unsigned const rep_G0_prob = enc->states.is_rep_G0[state];
    unsigned const rep0_long_prob = enc->states.is_rep0_long[state][pos_state];
    return GET_PRICE_0(rep_G0_prob) + GET_PRICE_0(rep0_long_prob);
}

static unsigned lzma_rep_price(lzma2_rmf_encoder* const enc, size_t const rep_index, size_t const state, size_t const pos_state)
{
    unsigned price;
    unsigned const rep_G0_prob = enc->states.is_rep_G0[state];
    if (rep_index == 0) {
        unsigned const rep0_long_prob = enc->states.is_rep0_long[state][pos_state];
        price = GET_PRICE_0(rep_G0_prob);
        price += GET_PRICE_1(rep0_long_prob);
    }
    else {
        unsigned const rep_G1_prob = enc->states.is_rep_G1[state];
        price = GET_PRICE_1(rep_G0_prob);
        if (rep_index == 1) {
            price += GET_PRICE_0(rep_G1_prob);
        }
        else {
            unsigned const rep_G2_prob = enc->states.is_rep_G2[state];
            price += GET_PRICE_1(rep_G1_prob);
            price += GET_PRICE(rep_G2_prob, rep_index - 2);
        }
    }
    return price;
}

static unsigned lzma_rep0_price(lzma2_rmf_encoder *const enc, size_t const len, size_t const state, size_t const pos_state)
{
    unsigned const rep_G0_prob = enc->states.is_rep_G0[state];
    unsigned const rep0_long_prob = enc->states.is_rep0_long[state][pos_state];
    return enc->states.rep_len_states.prices[pos_state][len - MATCH_LEN_MIN]
        + GET_PRICE_0(rep_G0_prob)
        + GET_PRICE_1(rep0_long_prob);
}

static unsigned lzma_literal_matched_price(const probability *const prob_table, uint32_t symbol, unsigned match_byte)
{
    unsigned price = 0;
    unsigned offs = 0x100;
    symbol |= 0x100;
    do {
        match_byte <<= 1;
        price += GET_PRICE(prob_table[offs + (match_byte & offs) + (symbol >> 8)], (symbol >> 7) & 1);
        symbol <<= 1;
        offs &= ~(match_byte ^ symbol);
    } while (symbol < 0x10000);
    return price;
}

HINT_INLINE
void lzma_encode_literal(lzma2_rmf_encoder *const enc, size_t const pos, uint32_t symbol, unsigned const prev_symbol)
{
    rcf_bit_0(&enc->rc, &enc->states.is_match[enc->states.state][pos & enc->pos_mask]);
    enc->states.state = literal_next_state(enc->states.state);

    probability* const prob_table = literal_prob_tbl(enc, pos, prev_symbol);
    symbol |= 0x100;
    do {
        rcf_bit(&enc->rc, prob_table + (symbol >> 8), symbol & (1 << 7));
        symbol <<= 1;
    } while (symbol < 0x10000);
}

HINT_INLINE
void lzma_encode_literal_matched(lzma2_rmf_encoder *const enc, const uint8_t* const data_block, size_t const pos, uint32_t symbol)
{
    rcf_bit_0(&enc->rc, &enc->states.is_match[enc->states.state][pos & enc->pos_mask]);
    enc->states.state = literal_next_state(enc->states.state);

    unsigned match_symbol = data_block[pos - enc->states.reps[0] - 1];
    probability* const prob_table = literal_prob_tbl(enc, pos, data_block[pos - 1]);
    unsigned offset = 0x100;
    symbol |= 0x100;
    do {
        match_symbol <<= 1;
        size_t prob_index = offset + (match_symbol & offset) + (symbol >> 8);
        rcf_bit(&enc->rc, prob_table + prob_index, symbol & (1 << 7));
        symbol <<= 1;
        offset &= ~(match_symbol ^ symbol);
    } while (symbol < 0x10000);
}

HINT_INLINE
void lzma_encode_literal_buf(lzma2_rmf_encoder *const enc, const uint8_t* const data_block, size_t const pos)
{
    uint32_t const symbol = data_block[pos];
    if (is_lit_state(enc->states.state)) {
        unsigned const prev_symbol = data_block[pos - 1];
        lzma_encode_literal(enc, pos, symbol, prev_symbol);
    }
    else {
        lzma_encode_literal_matched(enc, data_block, pos, symbol);
    }
}

static void lzma_len_set_prices(const probability *probs, uint32_t start_price, unsigned *prices)
{
    for (size_t i = 0; i < 8; i += 2) {
        uint32_t prob = probs[4 + (i >> 1)];
        uint32_t price = start_price + GET_PRICE(probs[1], (i >> 2))
            + GET_PRICE(probs[2 + (i >> 2)], (i >> 1) & 1);
        prices[i] = price + GET_PRICE_0(prob);
        prices[i + 1] = price + GET_PRICE_1(prob);
    }
}

FORCE_NOINLINE
static void lzma_len_update_prices(lzma2_rmf_encoder *const enc, lzma2_len_states* const ls)
{
    uint32_t b;

    {
        unsigned const prob = ls->choice;
        uint32_t a, c;
        b = GET_PRICE_1(prob);
        a = GET_PRICE_0(prob);
        c = b + GET_PRICE_0(ls->low[0]);
        for (size_t pos_state = 0; pos_state <= enc->pos_mask; pos_state++) {
            unsigned *const prices = ls->prices[pos_state];
            const probability *const probs = ls->low + (pos_state << (1 + LEN_LOW_BITS));
            lzma_len_set_prices(probs, a, prices);
            lzma_len_set_prices(probs + LEN_LOW_SYMBOLS, c, prices + LEN_LOW_SYMBOLS);
        }
    }

    size_t i = ls->table_size;

    if (i > LEN_LOW_SYMBOLS * 2) {
        const probability *const probs = ls->high;
        unsigned *const prices = ls->prices[0] + LEN_LOW_SYMBOLS * 2;
        i = (i - (LEN_LOW_SYMBOLS * 2 - 1)) >> 1;
        b += GET_PRICE_1(ls->low[0]);
        do {
            --i;
            size_t sym = i + (1 << (LEN_HIGH_BITS - 1));
            uint32_t price = b;
            do {
                size_t bit = sym & 1;
                sym >>= 1;
                price += GET_PRICE(probs[sym], bit);
            } while (sym >= 2);

            unsigned const prob = probs[i + (1 << (LEN_HIGH_BITS - 1))];
            prices[i * 2] = price + GET_PRICE_0(prob);
            prices[i * 2 + 1] = price + GET_PRICE_1(prob);
        } while (i);

        size_t const size = (ls->table_size - LEN_LOW_SYMBOLS * 2) * sizeof(ls->prices[0][0]);
        for (size_t pos_state = 1; pos_state <= enc->pos_mask; pos_state++)
            memcpy(ls->prices[pos_state] + LEN_LOW_SYMBOLS * 2, ls->prices[0] + LEN_LOW_SYMBOLS * 2, size);
    }
}

// Rare enough that not inlining is faster overall.
FORCE_NOINLINE
static void lzma_len_encode_mid_high(lzma2_rmf_encoder *const enc, lzma2_len_states* const len_prob_table, unsigned const len, size_t const pos_state)
{
    rcf_bit_1(&enc->rc, &len_prob_table->choice);
    if (len < LEN_LOW_SYMBOLS * 2) {
        rcf_bit_0(&enc->rc, &len_prob_table->low[0]);
        rcf_bittree(&enc->rc, len_prob_table->low + LEN_LOW_SYMBOLS + (pos_state << (1 + LEN_LOW_BITS)), LEN_LOW_BITS, len - LEN_LOW_SYMBOLS);
    }
    else {
        rcf_bit_1(&enc->rc, &len_prob_table->low[0]);
        rcf_bittree(&enc->rc, len_prob_table->high, LEN_HIGH_BITS, len - LEN_LOW_SYMBOLS * 2);
    }
}

HINT_INLINE
void lzma_len_encode(lzma2_rmf_encoder *const enc, lzma2_len_states* const len_prob_table, unsigned len, size_t const pos_state)
{
    len -= MATCH_LEN_MIN;
    if (len < LEN_LOW_SYMBOLS) {
        rcf_bit_0(&enc->rc, &len_prob_table->choice);
        rcf_bittree(&enc->rc, len_prob_table->low + (pos_state << (1 + LEN_LOW_BITS)), LEN_LOW_BITS, len);
    }
    else {
        lzma_len_encode_mid_high(enc, len_prob_table, len, pos_state);
    }
}

FORCE_NOINLINE
static void lzma_encode_rep_short(lzma2_rmf_encoder *const enc, size_t const pos_state)
{
    DEBUGLOG(7, "lzma_encode_rep_short");
    rcf_bit_1(&enc->rc, &enc->states.is_match[enc->states.state][pos_state]);
    rcf_bit_1(&enc->rc, &enc->states.is_rep[enc->states.state]);
    rcf_bit_0(&enc->rc, &enc->states.is_rep_G0[enc->states.state]);
    rcf_bit_0(&enc->rc, &enc->states.is_rep0_long[enc->states.state][pos_state]);
    enc->states.state = short_rep_next_state(enc->states.state);
}

FORCE_NOINLINE
static void lzma_encode_rep_long(lzma2_rmf_encoder *const enc, unsigned const len, unsigned const rep, size_t const pos_state)
{
    DEBUGLOG(7, "lzma_encode_rep_long : length %u, rep %u", len, rep);
    rcf_bit_1(&enc->rc, &enc->states.is_match[enc->states.state][pos_state]);
    rcf_bit_1(&enc->rc, &enc->states.is_rep[enc->states.state]);
    if (rep == 0) {
        rcf_bit_0(&enc->rc, &enc->states.is_rep_G0[enc->states.state]);
        rcf_bit_1(&enc->rc, &enc->states.is_rep0_long[enc->states.state][pos_state]);
    }
    else {
        uint32_t const distance = enc->states.reps[rep];
        rcf_bit_1(&enc->rc, &enc->states.is_rep_G0[enc->states.state]);
        if (rep == 1) {
            rcf_bit_0(&enc->rc, &enc->states.is_rep_G1[enc->states.state]);
        }
        else {
            rcf_bit_1(&enc->rc, &enc->states.is_rep_G1[enc->states.state]);
            rcf_bit(&enc->rc, &enc->states.is_rep_G2[enc->states.state], rep - 2);
            if (rep == 3)
                enc->states.reps[3] = enc->states.reps[2];
            enc->states.reps[2] = enc->states.reps[1];
        }
        enc->states.reps[1] = enc->states.reps[0];
        enc->states.reps[0] = distance;
    }
    lzma_len_encode(enc, &enc->states.rep_len_states, len, pos_state);
    enc->states.state = rep_next_state(enc->states.state);
    ++enc->rep_len_price_count;
}

HINT_INLINE
void lzma_encode_normal_match(lzma2_rmf_encoder *const enc, unsigned const len, uint32_t const dist, size_t const pos_state)
{
    DEBUGLOG(7, "lzma_encode_normal_match : length %u, dist %u", len, dist);
    rcf_bit_1(&enc->rc, &enc->states.is_match[enc->states.state][pos_state]);
    rcf_bit_0(&enc->rc, &enc->states.is_rep[enc->states.state]);
    enc->states.state = match_next_state(enc->states.state);

    lzma_len_encode(enc, &enc->states.len_states, len, pos_state);

    size_t const dist_slot = get_dist_slot(dist);
    rcf_bittree(&enc->rc, enc->states.dist_slot_encoders[len_to_dist_state(len)], DIST_SLOT_BITS, (unsigned)dist_slot);
    if (dist_slot >= DIST_MODEL_START) {
        unsigned const footer_bits = ((unsigned)(dist_slot >> 1) - 1);
        size_t const base = ((2 | (dist_slot & 1)) << footer_bits);
        unsigned const dist_reduced = (unsigned)(dist - base);
        if (dist_slot < DIST_MODEL_END) {
            rcf_bittree_reverse(&enc->rc, enc->states.dist_encoders + base - dist_slot - 1, footer_bits, dist_reduced);
        }
        else {
            rcf_direct(&enc->rc, dist_reduced >> ALIGN_BITS, footer_bits - ALIGN_BITS);
            rcf_bittree_reverse(&enc->rc, enc->states.dist_align_encoders, ALIGN_BITS, dist_reduced & ALIGN_MASK);
        }
    }
    enc->states.reps[3] = enc->states.reps[2];
    enc->states.reps[2] = enc->states.reps[1];
    enc->states.reps[1] = enc->states.reps[0];
    enc->states.reps[0] = dist;

    ++enc->match_price_count;
}

static inline size_t lzma_count(const uint8_t* cur, const uint8_t* match, const uint8_t* const end)
{
	size_t count = 0;
	size_t limit = end - cur;
	while (count < limit && cur[count] == match[count])
		++count;
	return count;
}

FORCE_INLINE_TEMPLATE
size_t lzma_encode_chunk_fast(lzma2_rmf_encoder *const enc,
    lzma_data_block const block,
    rmf_match_table* const tbl,
    int const struct_tbl,
    size_t pos,
    size_t const uncompressed_end)
{
    size_t const pos_mask = enc->pos_mask;
    size_t prev = pos;
    unsigned const search_depth = tbl->depth;

    while (pos < uncompressed_end && rc_chunk_size(&enc->rc) < enc->chunk_size) {
        size_t max_len;
        const uint8_t* data;
        /* Table of distance restrictions for short matches */
        static const uint32_t max_dist_table[] = { 0, 0, 0, 1 << 6, 1 << 14 };
        /* Get a match from the table, extended to its full length */
        rmf_match best_match = rmf_get_match(block, tbl, search_depth, struct_tbl, pos);
        if (best_match.length < MATCH_LEN_MIN) {
            ++pos;
            continue;
        }
        /* Use if near enough */
        if (best_match.length >= 5 || best_match.dist < max_dist_table[best_match.length])
            best_match.dist += REPS;
        else
            best_match.length = 0;

        max_len = my_min(MATCH_LEN_MAX, block.end - pos);
        data = block.data + pos;

        rmf_match best_rep = { 0, 0 };
        rmf_match rep_match;
        /* Search all of the rep distances */
        for (rep_match.dist = 0; rep_match.dist < REPS; ++rep_match.dist) {
            const uint8_t *data_2 = data - enc->states.reps[rep_match.dist] - 1;
            if (not_equal_16(data, data_2))
                continue;

            rep_match.length = (uint32_t)(lzma_count(data + 2, data_2 + 2, data + max_len) + 2);
            if (rep_match.length >= max_len) {
                best_match = rep_match;
                goto _encode;
            }
            if (rep_match.length > best_rep.length)
                best_rep = rep_match;
        }
        /* Encode if it is MATCH_LEN_MAX or completes the block */
        if (best_match.length >= max_len)
            goto _encode;

        if (best_rep.length >= 2) {
            if (best_rep.length > best_match.length) {
                best_match = best_rep;
            }
            else {
                /* Modified ZSTD scheme for estimating cost */
                int const gain2 = (int)(best_rep.length * 3 - best_rep.dist);
                int const gain1 = (int)(best_match.length * 3 - bsr32(best_match.dist + 1) + 1);
                if (gain2 > gain1) {
                    DEBUGLOG(7, "Replace match (%u, %u) with rep (%u, %u)", best_match.length, best_match.dist, best_rep.length, best_rep.dist);
                    best_match = best_rep;
                }
            }
        }

        if (best_match.length < MATCH_LEN_MIN) {
            ++pos;
            continue;
        }

        for (size_t next = pos + 1; best_match.length < MATCH_LEN_MAX && next < uncompressed_end; ++next) {
            /* lazy matching scheme from ZSTD */
            rmf_match next_match = rmf_get_next_match(block, tbl, search_depth, struct_tbl, next);
            if (next_match.length >= MATCH_LEN_MIN) {
                best_rep.length = 0;
                data = block.data + next;
                max_len = my_min(MATCH_LEN_MAX, block.end - next);
                for (rep_match.dist = 0; rep_match.dist < REPS; ++rep_match.dist) {
                    const uint8_t *data_2 = data - enc->states.reps[rep_match.dist] - 1;
                    if (not_equal_16(data, data_2))
                        continue;

                    rep_match.length = (uint32_t)(lzma_count(data + 2, data_2 + 2, data + max_len) + 2);
                    if (rep_match.length > best_rep.length)
                        best_rep = rep_match;
                }
                if (best_rep.length >= 3) {
                    int const gain2 = (int)(best_rep.length * 3 - best_rep.dist);
                    int const gain1 = (int)(best_match.length * 3 - bsr32(best_match.dist + 1) + 1);
                    if (gain2 > gain1) {
                        DEBUGLOG(7, "Replace match (%u, %u) with rep (%u, %u)", best_match.length, best_match.dist, best_rep.length, best_rep.dist);
                        best_match = best_rep;
                        pos = next;
                    }
                }
                if (next_match.length >= 3 && next_match.dist != best_match.dist) {
                    int const gain2 = (int)(next_match.length * 4 - bsr32(next_match.dist + 1));   /* raw approx */
                    int const gain1 = (int)(best_match.length * 4 - bsr32(best_match.dist + 1) + 4);
                    if (gain2 > gain1) {
                        DEBUGLOG(7, "Replace match (%u, %u) with match (%u, %u)", best_match.length, best_match.dist, next_match.length, next_match.dist + REPS);
                        best_match = next_match;
                        best_match.dist += REPS;
                        pos = next;
                        continue;
                    }
                }
            }
            ++next;
            /* Recheck next < uncompressed_end. uncompressed_end could be block.end so decrementing the max chunk size won't obviate the need. */
            if (next >= uncompressed_end)
                break;

            next_match = rmf_get_next_match(block, tbl, search_depth, struct_tbl, next);
            if (next_match.length < 4)
                break;

            data = block.data + next;
            max_len = my_min(MATCH_LEN_MAX, block.end - next);
            best_rep.length = 0;

            for (rep_match.dist = 0; rep_match.dist < REPS; ++rep_match.dist) {
                const uint8_t *data_2 = data - enc->states.reps[rep_match.dist] - 1;
                if (not_equal_16(data, data_2))
                    continue;

                rep_match.length = (uint32_t)(lzma_count(data + 2, data_2 + 2, data + max_len) + 2);
                if (rep_match.length > best_rep.length)
                    best_rep = rep_match;
            }
            if (best_rep.length >= 4) {
                int const gain2 = (int)(best_rep.length * 4 - (best_rep.dist >> 1));
                int const gain1 = (int)(best_match.length * 4 - bsr32(best_match.dist + 1) + 1);
                if (gain2 > gain1) {
                    DEBUGLOG(7, "Replace match (%u, %u) with rep (%u, %u)", best_match.length, best_match.dist, best_rep.length, best_rep.dist);
                    best_match = best_rep;
                    pos = next;
                }
            }
            if (next_match.dist != best_match.dist) {
                int const gain2 = (int)(next_match.length * 4 - bsr32(next_match.dist + 1));
                int const gain1 = (int)(best_match.length * 4 - bsr32(best_match.dist + 1) + 7);
                if (gain2 > gain1) {
                    DEBUGLOG(7, "Replace match (%u, %u) with match (%u, %u)", best_match.length, best_match.dist, next_match.length, next_match.dist + REPS);
                    best_match = next_match;
                    best_match.dist += REPS;
                    pos = next;
                    continue;
                }
            }

            break;
        }
_encode:
        assert(pos + best_match.length <= block.end);

        while (prev < pos) {
            if (enc->rc.out_index >= enc->chunk_limit)
                return prev;

            if (block.data[prev] != block.data[prev - enc->states.reps[0] - 1]) {
                lzma_encode_literal_buf(enc, block.data, prev);
                ++prev;
            }
            else {
                lzma_encode_rep_short(enc, prev & pos_mask);
                ++prev;
            }
        }

        if(best_match.length >= MATCH_LEN_MIN) {
            if (best_match.dist >= REPS) {
                lzma_encode_normal_match(enc, best_match.length, best_match.dist - REPS, pos & pos_mask);
                pos += best_match.length;
                prev = pos;
            }
            else {
                lzma_encode_rep_long(enc, best_match.length, best_match.dist, pos & pos_mask);
                pos += best_match.length;
                prev = pos;
            }
        }
    }
    while (prev < pos && enc->rc.out_index < enc->chunk_limit) {
        if (block.data[prev] != block.data[prev - enc->states.reps[0] - 1])
            lzma_encode_literal_buf(enc, block.data, prev);
        else
            lzma_encode_rep_short(enc, prev & pos_mask);
        ++prev;
    }
    return prev;
}

/*
 * Reverse the direction of the linked list generated by the optimal parser
 */
FORCE_NOINLINE
static void lzma_reverse_optimal_chain(lzma2_node* const opt_buf, size_t cur)
{
    unsigned len = (unsigned)opt_buf[cur].len;
    uint32_t dist = opt_buf[cur].dist;

    for(;;) {
        unsigned const extra = (unsigned)opt_buf[cur].extra;
        cur -= len;

        if (extra) {
            opt_buf[cur].len = (uint32_t)len;
            len = extra;
            if (extra == 1) {
                opt_buf[cur].dist = dist;
                dist = NULL_DIST;
                --cur;
            }
            else {
                opt_buf[cur].dist = 0;
                --cur;
                --len;
                opt_buf[cur].dist = NULL_DIST;
                opt_buf[cur].len = 1;
                cur -= len;
            }
        }

        unsigned const next_len = opt_buf[cur].len;
        uint32_t const next_dist = opt_buf[cur].dist;

        opt_buf[cur].dist = dist;
        opt_buf[cur].len = (uint32_t)len;

        if (cur == 0)
            break;

        len = next_len;
        dist = next_dist;
    }
}

static unsigned lzma_literal_price(lzma2_rmf_encoder *const enc, size_t const pos, size_t const state, unsigned const prev_symbol, uint32_t symbol, unsigned const match_byte)
{
    const probability* const prob_table = literal_prob_tbl(enc, pos, prev_symbol);
    if (is_lit_state(state)) {
        unsigned price = 0;
        symbol |= 0x100;
        do {
            price += GET_PRICE(prob_table[symbol >> 8], (symbol >> 7) & 1);
            symbol <<= 1;
        } while (symbol < 0x10000);
        return price;
    }
    return lzma_literal_matched_price(prob_table, symbol, match_byte);
}

/* 
 * Reset the hash object for encoding a new slice of a block
 */
static void lzma_hash_reset(lzma2_rmf_encoder *const enc, unsigned const dictionary_bits_3)
{
    enc->hash_dict_3 = (ptrdiff_t)1 << dictionary_bits_3;
    enc->chain_mask_3 = enc->hash_dict_3 - 1;
    memset(enc->hash_buf->table_3, 0xFF, sizeof(enc->hash_buf->table_3));
}

/*
 * Create hash table and chain with dict size dictionary_bits_3. Frees any existing object.
 */
static int lzma_hash_create(lzma2_rmf_encoder *const enc, unsigned const dictionary_bits_3)
{
    DEBUGLOG(3, "Create hash chain : dict bits %u", dictionary_bits_3);

    if (enc->hash_buf)
        free(enc->hash_buf);

    enc->hash_alloc_3 = (ptrdiff_t)1 << dictionary_bits_3;
    enc->hash_buf = malloc(sizeof(lzma2_hc3) + (enc->hash_alloc_3 - 1) * sizeof(int32_t));

    if (enc->hash_buf == NULL)
        return 1;

    lzma_hash_reset(enc, dictionary_bits_3);

    return 0;
}

/* Create a hash chain for hybrid mode if options require one.
 * Used for allocating before compression begins. Any existing table will be reused if
 * it is at least as large as required.
 */
int lzma2_rmf_hash_alloc(lzma2_rmf_encoder *const enc, const lzma_options_lzma* const options)
{
    if (enc->strategy == LZMA_MODE_ULTRA && enc->hash_alloc_3 < ((ptrdiff_t)1 << options->near_dict_size_log))
        return lzma_hash_create(enc, options->near_dict_size_log);

    return 0;
}

#ifdef TUKLIB_FAST_UNALIGNED_ACCESS
#  ifdef WORDS_BIGENDIAN
#    define GET_HASH_3(data) (((*(uint32_t*)(data) & 0xFFFFFF00) * 506832829U) >> (32 - HC3_BITS))
#  else
#    define GET_HASH_3(data) (((*(uint32_t*)(data) << 8) * 506832829U) >> (32 - HC3_BITS))
#  endif
#else
#  define GET_HASH_3(data) (((((unsigned)((data)[0]) << 8) | ((data)[1] << 16) | ((data)[2] << 24)) * 506832829U) >> (32 - HC3_BITS))
#endif

/* Find matches nearer than the match from the RMF. If none is at least as long as
 * the RMF match (most likely), insert that match at the end of the list.
 */
HINT_INLINE
size_t lzma_hash_match(lzma2_rmf_encoder *const enc, lzma_data_block const block,
    ptrdiff_t const pos,
    size_t const length_limit,
    rmf_match const match)
{
    ptrdiff_t const hash_dict_3 = enc->hash_dict_3;
    const uint8_t* data = block.data;
    lzma2_hc3* const tbl = enc->hash_buf;
    ptrdiff_t const chain_mask_3 = enc->chain_mask_3;

    enc->match_count = 0;
    enc->hash_prev_index = my_max(enc->hash_prev_index, pos - hash_dict_3);
    /* Update hash tables and chains for any positions that were skipped */
    while (++enc->hash_prev_index < pos) {
        size_t hash = GET_HASH_3(data + enc->hash_prev_index);
        tbl->hash_chain_3[enc->hash_prev_index & chain_mask_3] = tbl->table_3[hash];
        tbl->table_3[hash] = (int32_t)enc->hash_prev_index;
    }
    data += pos;

    size_t const hash = GET_HASH_3(data);
    ptrdiff_t const first_3 = tbl->table_3[hash];
    tbl->table_3[hash] = (int32_t)pos;

    size_t max_len = 2;

    if (first_3 >= 0) {
        int cycles = enc->match_cycles;
        ptrdiff_t const end_index = pos - (((ptrdiff_t)match.dist < hash_dict_3)
			? (ptrdiff_t)match.dist : hash_dict_3);
        ptrdiff_t match_3 = first_3;
        if (match_3 >= end_index) {
            do {
                --cycles;
                const uint8_t* data_2 = block.data + match_3;
                size_t len_test = lzma_count(data + 1, data_2 + 1, data + length_limit) + 1;
                if (len_test > max_len) {
                    enc->matches[enc->match_count].length = (uint32_t)len_test;
                    enc->matches[enc->match_count].dist = (uint32_t)(pos - match_3 - 1);
                    ++enc->match_count;
                    max_len = len_test;
                    if (len_test >= length_limit)
                        break;
                }
                if (cycles <= 0)
                    break;
                match_3 = tbl->hash_chain_3[match_3 & chain_mask_3];
            } while (match_3 >= end_index);
        }
    }
    tbl->hash_chain_3[pos & chain_mask_3] = (int32_t)first_3;
    if ((unsigned)max_len < match.length) {
        /* Insert the match from the RMF */
        enc->matches[enc->match_count] = match;
        ++enc->match_count;
        return match.length;
    }
    return max_len;
}

/* The speed of this function is critical. The sections have many variables
* in common, so breaking it up into shorter functions is not feasible.
* For each position cur, starting at 1, check some or all possible
* encoding choices - a literal, 1-byte rep 0 match, all rep match lengths, and
* all match lengths at available distances. It also checks the combined
* sequences literal+rep0, rep+lit+rep0 and match+lit+rep0.
* If is_hybrid != 0, this method works in hybrid mode, using the
* hash chain to find shorter matches at near distances. */
FORCE_INLINE_TEMPLATE
size_t lzma_optimal_parse(lzma2_rmf_encoder* const enc, lzma_data_block const block,
    rmf_match match,
    size_t const pos,
    size_t const cur,
    size_t len_end,
    int const is_hybrid,
    uint32_t* const reps)
{
    lzma2_node* const cur_opt = &enc->opt_buf[cur];
    size_t const pos_mask = enc->pos_mask;
    size_t const pos_state = (pos & pos_mask);
    const uint8_t* const data = block.data + pos;
    size_t const fast_length = enc->fast_length;
    size_t prev_index = cur - cur_opt->len;
    size_t state;
    size_t bytes_avail;
    uint32_t match_price;
    uint32_t rep_match_price;

    /* Update the states according to how this location was reached */
    if (cur_opt->len == 1) {
        /* Literal or 1-byte rep */
        const uint8_t *next_state = (cur_opt->dist == 0) ? short_rep_next_tbl : lit_next_tbl;
        state = next_state[enc->opt_buf[prev_index].state];
    }
    else {
        /* Match or rep match */
        size_t const dist = cur_opt->dist;

        if (cur_opt->extra) {
            prev_index -= cur_opt->extra;
            state = STATE_REP_AFTER_LIT - ((dist >= REPS) & (cur_opt->extra == 1));
        }
        else {
            state = enc->opt_buf[prev_index].state;
            state = match_next_state(state) + (dist < REPS);
        }
        const lzma2_node *const prev_opt = &enc->opt_buf[prev_index];
        if (dist < REPS) {
            /* Move the chosen rep to the front.
             * The table is hideous but faster than branching :D */
            reps[0] = prev_opt->reps[dist];
            size_t table = 1 | (2 << 2) | (3 << 4)
                | (0 << 8) | (2 << 10) | (3 << 12)
                | (0L << 16) | (1L << 18) | (3L << 20)
                | (0L << 24) | (1L << 26) | (2L << 28);
            table >>= (dist << 3);
            reps[1] = prev_opt->reps[table & 3];
            table >>= 2;
            reps[2] = prev_opt->reps[table & 3];
            table >>= 2;
            reps[3] = prev_opt->reps[table & 3];
        }
        else {
            reps[0] = (uint32_t)(dist - REPS);
            reps[1] = prev_opt->reps[0];
            reps[2] = prev_opt->reps[1];
            reps[3] = prev_opt->reps[2];
        }
    }
    cur_opt->state = state;
    memcpy(cur_opt->reps, reps, sizeof(cur_opt->reps));
    probability const is_rep_prob = enc->states.is_rep[state];

    {   lzma2_node *const next_opt = &enc->opt_buf[cur + 1];
        uint32_t const cur_price = cur_opt->price;
        uint32_t const next_price = next_opt->price;
        probability const is_match_prob = enc->states.is_match[state][pos_state];
        unsigned const cur_byte = *data;
        unsigned const match_byte = *(data - reps[0] - 1);
       
        uint32_t cur_and_lit_price = cur_price + GET_PRICE_0(is_match_prob);
        /* This is a compromise to try to filter out cases where literal + rep0 is unlikely to be cheaper */
        uint8_t try_lit = cur_and_lit_price + MIN_LITERAL_PRICE / 2U <= next_price;
        if (try_lit) {
            /* cur_and_lit_price is used later for the literal + rep0 test */
            cur_and_lit_price += lzma_literal_price(enc, pos, state, data[-1], cur_byte, match_byte);
            /* Try literal */
            if (cur_and_lit_price < next_price) {
                next_opt->price = cur_and_lit_price;
                next_opt->len = 1;
                mark_literal(*next_opt);
                if (is_hybrid) /* Evaluates as a constant expression due to inlining */
                    try_lit = 0;
            }
        }
        match_price = cur_price + GET_PRICE_1(is_match_prob);
        rep_match_price = match_price + GET_PRICE_1(is_rep_prob);
        if (match_byte == cur_byte) {
            /* Try 1-byte rep0 */
            uint32_t short_rep_price = rep_match_price + lzma_rep_1_price(enc, state, pos_state);
            if (short_rep_price <= next_opt->price) {
                next_opt->price = short_rep_price;
                next_opt->len = 1;
                mark_short_rep(*next_opt);
            }
        }
        bytes_avail = my_min(block.end - pos, OPT_BUF_SIZE - 1 - cur);
        if (bytes_avail < 2)
            return len_end;

        /* If match_byte == cur_byte a rep0 begins at the current position */
        if (is_hybrid && try_lit && match_byte != cur_byte) {
            /* Try literal + rep0 */
            const uint8_t *const data_2 = data - reps[0];
            size_t limit = my_min(bytes_avail - 1, fast_length);
            size_t len_test_2 = lzma_count(data + 1, data_2, data + 1 + limit);
            if (len_test_2 >= 2) {
                size_t const state_2 = literal_next_state(state);
                size_t const pos_state_next = (pos + 1) & pos_mask;
                uint32_t const next_rep_match_price = cur_and_lit_price +
                    GET_PRICE_1(enc->states.is_match[state_2][pos_state_next]) +
                    GET_PRICE_1(enc->states.is_rep[state_2]);
                uint32_t const cur_and_len_price = next_rep_match_price + lzma_rep0_price(enc, len_test_2, state_2, pos_state_next);
                size_t const offset = cur + 1 + len_test_2;
                if (cur_and_len_price < enc->opt_buf[offset].price) {
                    len_end = my_max(len_end, offset);
                    enc->opt_buf[offset].price = cur_and_len_price;
                    enc->opt_buf[offset].len = (unsigned)len_test_2;
                    enc->opt_buf[offset].dist = 0;
                    enc->opt_buf[offset].extra = 1;
                }
            }
        }
    }

    size_t const max_length = my_min(bytes_avail, fast_length);
    size_t start_len = 2;

    if (match.length > 0) {
        size_t len_test;
        size_t len;
        uint32_t cur_rep_price;
        for (size_t rep_index = 0; rep_index < REPS; ++rep_index) {
            const uint8_t *const data_2 = data - reps[rep_index] - 1;
            if (not_equal_16(data, data_2))
                continue;
            /* Test is limited to fast_length, but it is rare for the RMF to miss the longest match,
             * therefore this function is rarely called when a rep len > fast_length exists */
            len_test = lzma_count(data + 2, data_2 + 2, data + max_length) + 2;
            len_end = my_max(len_end, cur + len_test);
            cur_rep_price = rep_match_price + lzma_rep_price(enc, rep_index, state, pos_state);
            len = 2;
            /* Try rep match */
            do {
                uint32_t const cur_and_len_price = cur_rep_price + enc->states.rep_len_states.prices[pos_state][len - MATCH_LEN_MIN];
                lzma2_node *const opt = &enc->opt_buf[cur + len];
                if (cur_and_len_price < opt->price) {
                    opt->price = cur_and_len_price;
                    opt->len = (unsigned)len;
                    opt->dist = (uint32_t)rep_index;
                    opt->extra = 0;
                }
            } while (++len <= len_test);

            if (rep_index == 0) {
                /* Save time by exluding normal matches not longer than the rep */
                start_len = len_test + 1;
            }
            /* rep + literal + rep0 is not common so this test is skipped for faster, non-hybrid encoding */
            if (is_hybrid && len_test + 3 <= bytes_avail && !not_equal_16(data + len_test + 1, data_2 + len_test + 1)) {
                /* Try rep + literal + rep0.
                 * The second rep may be > fast_length, but it is not worth the extra time to handle this case
                 * and the price table is not filled for it */
                size_t const len_test_2 = lzma_count(data + len_test + 3,
                    data_2 + len_test + 3,
                    data + my_min(len_test + 1 + fast_length, bytes_avail)) + 2;
                size_t state_2 = rep_next_state(state);
                size_t pos_state_next = (pos + len_test) & pos_mask;
                uint32_t rep_lit_rep_total_price =
                    cur_rep_price + enc->states.rep_len_states.prices[pos_state][len_test - MATCH_LEN_MIN]
                    + GET_PRICE_0(enc->states.is_match[state_2][pos_state_next])
                    + lzma_literal_matched_price(literal_prob_tbl(enc, pos + len_test, data[len_test - 1]),
                        data[len_test], data_2[len_test]);

                state_2 = STATE_LIT_AFTER_REP;
                pos_state_next = (pos + len_test + 1) & pos_mask;
                rep_lit_rep_total_price +=
                    GET_PRICE_1(enc->states.is_match[state_2][pos_state_next]) +
                    GET_PRICE_1(enc->states.is_rep[state_2]);
                size_t const offset = cur + len_test + 1 + len_test_2;
                rep_lit_rep_total_price += lzma_rep0_price(enc, len_test_2, state_2, pos_state_next);
                if (rep_lit_rep_total_price < enc->opt_buf[offset].price) {
                    len_end = my_max(len_end, offset);
                    enc->opt_buf[offset].price = rep_lit_rep_total_price;
                    enc->opt_buf[offset].len = (unsigned)len_test_2;
                    enc->opt_buf[offset].dist = (uint32_t)rep_index;
                    enc->opt_buf[offset].extra = (unsigned)(len_test + 1);
                }
            }
        }
    }
    if (match.length >= start_len && max_length >= start_len) {
        /* Try normal match */
        uint32_t const normal_match_price = match_price + GET_PRICE_0(is_rep_prob);
        if (!is_hybrid) {
            /* Normal mode - single match */
            size_t const length = my_min(match.length, max_length);
            size_t const cur_dist = match.dist;
            size_t const dist_slot = get_dist_slot(match.dist);
            size_t len_test = length;
            len_end = my_max(len_end, cur + length);
            for (; len_test >= start_len; --len_test) {
                uint32_t cur_and_len_price = normal_match_price + enc->states.len_states.prices[pos_state][len_test - MATCH_LEN_MIN];
                size_t const len_to_dist_state = len_to_dist_state(len_test);

                if (cur_dist < FULL_DISTANCES)
                    cur_and_len_price += enc->distance_prices[len_to_dist_state][cur_dist];
                else 
                    cur_and_len_price += enc->dist_slot_prices[len_to_dist_state][dist_slot] + enc->align_prices[cur_dist & ALIGN_MASK];

                lzma2_node *const opt = &enc->opt_buf[cur + len_test];
                if (cur_and_len_price < opt->price) {
                    opt->price = cur_and_len_price;
                    opt->len = (unsigned)len_test;
                    opt->dist = (uint32_t)(cur_dist + REPS);
                    opt->extra = 0;
                }
                else break;
            }
        }
        else {
            /* Hybrid mode */
            size_t main_len;

            match.length = my_min(match.length, (uint32_t)max_length);
            /* Need to test max_length < 4 because the hash fn reads a uint32_t */
            if (match.length < 3 || max_length < 4) {
                enc->matches[0] = match;
                enc->match_count = 1;
                main_len = match.length;
            }
            else {
                main_len = lzma_hash_match(enc, block, pos, max_length, match);
            }
            ptrdiff_t match_index = enc->match_count - 1;
            len_end = my_max(len_end, cur + main_len);

            /* Start with a match longer than the best rep if one exists */
            ptrdiff_t start_match = 0;
            while (start_len > enc->matches[start_match].length)
                ++start_match;

            enc->matches[start_match - 1].length = (uint32_t)start_len - 1; /* Avoids an if..else branch in the loop. [-1] is ok */

            for (; match_index >= start_match; --match_index) {
                size_t len_test = enc->matches[match_index].length;
                size_t const cur_dist = enc->matches[match_index].dist;
                const uint8_t *const data_2 = data - cur_dist - 1;
                size_t const rep_0_pos = len_test + 1;
                size_t dist_slot = get_dist_slot((uint32_t)cur_dist);
                uint32_t cur_and_len_price;
                /* Test from the full length down to 1 more than the next shorter match */
                size_t base_len = enc->matches[match_index - 1].length + 1;
                for (; len_test >= base_len; --len_test) {
                    cur_and_len_price = normal_match_price + enc->states.len_states.prices[pos_state][len_test - MATCH_LEN_MIN];
                    size_t const len_to_dist_state = len_to_dist_state(len_test);
                    if (cur_dist < FULL_DISTANCES)
                        cur_and_len_price += enc->distance_prices[len_to_dist_state][cur_dist];
                    else
                        cur_and_len_price += enc->dist_slot_prices[len_to_dist_state][dist_slot] + enc->align_prices[cur_dist & ALIGN_MASK];

                    uint8_t const sub_len = len_test < enc->matches[match_index].length;

                    lzma2_node *const opt = &enc->opt_buf[cur + len_test];
                    if (cur_and_len_price < opt->price) {
                        opt->price = cur_and_len_price;
                        opt->len = (unsigned)len_test;
                        opt->dist = (uint32_t)(cur_dist + REPS);
                        opt->extra = 0;
                    }
                    else if(sub_len)
                        break; /* End the tests if prices for shorter lengths are not lower than those already recorded */

                    if (!sub_len && rep_0_pos + 2 <= bytes_avail && !not_equal_16(data + rep_0_pos, data_2 + rep_0_pos)) {
                        /* Try match + literal + rep0 */
                        size_t const limit = my_min(rep_0_pos + fast_length, bytes_avail);
                        size_t const len_test_2 = lzma_count(data + rep_0_pos + 2, data_2 + rep_0_pos + 2, data + limit) + 2;
                        size_t state_2 = match_next_state(state);
                        size_t pos_state_next = (pos + len_test) & pos_mask;
                        uint32_t match_lit_rep_total_price = cur_and_len_price +
                            GET_PRICE_0(enc->states.is_match[state_2][pos_state_next]) +
                            lzma_literal_matched_price(literal_prob_tbl(enc, pos + len_test, data[len_test - 1]),
                                data[len_test], data_2[len_test]);

                        state_2 = STATE_LIT_AFTER_MATCH;
                        pos_state_next = (pos_state_next + 1) & pos_mask;
                        match_lit_rep_total_price +=
                            GET_PRICE_1(enc->states.is_match[state_2][pos_state_next]) +
                            GET_PRICE_1(enc->states.is_rep[state_2]);
                        size_t const offset = cur + rep_0_pos + len_test_2;
                        match_lit_rep_total_price += lzma_rep0_price(enc, len_test_2, state_2, pos_state_next);
                        if (match_lit_rep_total_price < enc->opt_buf[offset].price) {
                            len_end = my_max(len_end, offset);
                            enc->opt_buf[offset].price = match_lit_rep_total_price;
                            enc->opt_buf[offset].len = (unsigned)len_test_2;
                            enc->opt_buf[offset].extra = (unsigned)rep_0_pos;
                            enc->opt_buf[offset].dist = (uint32_t)(cur_dist + REPS);
                        }
                    }
                }
            }
        }
    }
    return len_end;
}

FORCE_NOINLINE
static void lzma_init_matches_pos0(lzma2_rmf_encoder *const enc,
    rmf_match const match,
    size_t const pos_state,
    size_t len,
    unsigned const normal_match_price)
{
    if ((unsigned)len <= match.length) {
        size_t const distance = match.dist;
        size_t const slot = get_dist_slot(match.dist);
        /* Test every available length of the match */
        do {
            unsigned cur_and_len_price = normal_match_price + enc->states.len_states.prices[pos_state][len - MATCH_LEN_MIN];
            size_t const len_to_dist_state = len_to_dist_state(len);

            if (distance < FULL_DISTANCES)
                cur_and_len_price += enc->distance_prices[len_to_dist_state][distance];
            else
                cur_and_len_price += enc->align_prices[distance & ALIGN_MASK] + enc->dist_slot_prices[len_to_dist_state][slot];

            if (cur_and_len_price < enc->opt_buf[len].price) {
                enc->opt_buf[len].price = cur_and_len_price;
                enc->opt_buf[len].len = (unsigned)len;
                enc->opt_buf[len].dist = (uint32_t)(distance + REPS);
                enc->opt_buf[len].extra = 0;
            }
            ++len;
        } while ((uint32_t)len <= match.length);
    }
}

FORCE_NOINLINE
static size_t lzma_init_matches_pos0_best(lzma2_rmf_encoder *const enc, lzma_data_block const block,
    rmf_match const match,
    size_t const pos,
    size_t start_len,
    unsigned const normal_match_price)
{
    if (start_len <= match.length) {
        size_t main_len;
        if (match.length < 3 || block.end - pos < 4) {
            enc->matches[0] = match;
            enc->match_count = 1;
            main_len = match.length;
        }
        else {
            main_len = lzma_hash_match(enc, block, pos, my_min(block.end - pos, enc->fast_length), match);
        }

        ptrdiff_t start_match = 0;
        while (start_len > enc->matches[start_match].length)
            ++start_match;

        enc->matches[start_match - 1].length = (uint32_t)start_len - 1; /* Avoids an if..else branch in the loop. [-1] is ok */

        size_t pos_state = pos & enc->pos_mask;

        for (ptrdiff_t match_index = enc->match_count - 1; match_index >= start_match; --match_index) {
            size_t len_test = enc->matches[match_index].length;
            size_t const distance = enc->matches[match_index].dist;
            size_t const slot = get_dist_slot((uint32_t)distance);
            size_t const base_len = enc->matches[match_index - 1].length + 1;
            /* Test every available match length at the shortest distance. The buffer is sorted */
            /* in order of increasing length, and therefore increasing distance too. */
            for (; len_test >= base_len; --len_test) {
                unsigned cur_and_len_price = normal_match_price
                    + enc->states.len_states.prices[pos_state][len_test - MATCH_LEN_MIN];
                size_t const len_to_dist_state = len_to_dist_state(len_test);

                if (distance < FULL_DISTANCES)
                    cur_and_len_price += enc->distance_prices[len_to_dist_state][distance];
                else
                    cur_and_len_price += enc->align_prices[distance & ALIGN_MASK] + enc->dist_slot_prices[len_to_dist_state][slot];

                if (cur_and_len_price < enc->opt_buf[len_test].price) {
                    enc->opt_buf[len_test].price = cur_and_len_price;
                    enc->opt_buf[len_test].len = (unsigned)len_test;
                    enc->opt_buf[len_test].dist = (uint32_t)(distance + REPS);
                    enc->opt_buf[len_test].extra = 0;
                }
                else break;
            }
        }
        return main_len;
    }
    return 0;
}

/* Test all available options at position 0 of the optimizer buffer.
* The prices at this point are all initialized to RC_INFINITY_PRICE.
* This function must not be called at a position where no match is
* available. */
FORCE_INLINE_TEMPLATE
size_t lzma_init_optimizer_pos0(lzma2_rmf_encoder *const enc, lzma_data_block const block,
    rmf_match const match,
    size_t const pos,
    int const is_hybrid,
    uint32_t* const reps)
{
    size_t const max_length = my_min(block.end - pos, MATCH_LEN_MAX);
    const uint8_t *const data = block.data + pos;
    const uint8_t *data_2;
    size_t rep_max_index = 0;
    size_t rep_lens[REPS];

    /* Find any rep matches */
    for (size_t i = 0; i < REPS; ++i) {
        reps[i] = enc->states.reps[i];
        data_2 = data - reps[i] - 1;
        if (not_equal_16(data, data_2)) {
            rep_lens[i] = 0;
            continue;
        }
        rep_lens[i] = lzma_count(data + 2, data_2 + 2, data + max_length) + 2;
        if (rep_lens[i] > rep_lens[rep_max_index])
            rep_max_index = i;
    }
    if (rep_lens[rep_max_index] >= enc->fast_length) {
        enc->opt_buf[0].len = (unsigned)(rep_lens[rep_max_index]);
        enc->opt_buf[0].dist = (uint32_t)rep_max_index;
        return 0;
    }
    if (match.length >= enc->fast_length) {
        enc->opt_buf[0].len = match.length;
        enc->opt_buf[0].dist = match.dist + REPS;
        return 0;
    }

    unsigned const cur_byte = *data;
    unsigned const match_byte = *(data - reps[0] - 1);
    size_t const state = enc->states.state;
    size_t const pos_state = pos & enc->pos_mask;
    probability const is_match_prob = enc->states.is_match[state][pos_state];
    probability const is_rep_prob = enc->states.is_rep[state];

    enc->opt_buf[0].state = state;
    /* Set the price for literal */
    enc->opt_buf[1].price = GET_PRICE_0(is_match_prob) +
        lzma_literal_price(enc, pos, state, data[-1], cur_byte, match_byte);
    mark_literal(enc->opt_buf[1]);

    unsigned const match_price = GET_PRICE_1(is_match_prob);
    unsigned const rep_match_price = match_price + GET_PRICE_1(is_rep_prob);
    if (match_byte == cur_byte) {
        /* Try 1-byte rep0 */
        unsigned const short_rep_price = rep_match_price + lzma_rep_1_price(enc, state, pos_state);
        if (short_rep_price < enc->opt_buf[1].price) {
            enc->opt_buf[1].price = short_rep_price;
            mark_short_rep(enc->opt_buf[1]);
        }
    }
    memcpy(enc->opt_buf[0].reps, reps, sizeof(enc->opt_buf[0].reps));
    enc->opt_buf[1].len = 1;
    /* Test the rep match prices */
    for (size_t i = 0; i < REPS; ++i) {
        size_t rep_len = rep_lens[i];
        if (rep_len < 2)
            continue;

        unsigned const price = rep_match_price + lzma_rep_price(enc, i, state, pos_state);
        /* Test every available length of the rep */
        do {
            unsigned const cur_and_len_price = price + enc->states.rep_len_states.prices[pos_state][rep_len - MATCH_LEN_MIN];
            if (cur_and_len_price < enc->opt_buf[rep_len].price) {
                enc->opt_buf[rep_len].price = cur_and_len_price;
                enc->opt_buf[rep_len].len = (unsigned)rep_len;
                enc->opt_buf[rep_len].dist = (uint32_t)i;
                enc->opt_buf[rep_len].extra = 0;
            }
        } while (--rep_len >= MATCH_LEN_MIN);
    }
    unsigned const normal_match_price = match_price + GET_PRICE_0(is_rep_prob);
    size_t const len = (rep_lens[0] >= 2) ? rep_lens[0] + 1 : 2;
    /* Test the match prices */
    if (!is_hybrid) {
        /* Normal mode */
        lzma_init_matches_pos0(enc, match, pos_state, len, normal_match_price);
        return my_max(match.length, rep_lens[rep_max_index]);
    }
    else {
        /* Hybrid mode */
        size_t main_len = lzma_init_matches_pos0_best(enc, block, match, pos, len, normal_match_price);
        return my_max(main_len, rep_lens[rep_max_index]);
    }
}

FORCE_INLINE_TEMPLATE
size_t lzma_encode_opt_sequence(lzma2_rmf_encoder *const enc, lzma_data_block const block,
    rmf_match_table* const tbl,
    int const struct_tbl,
    int const is_hybrid,
    size_t start_index,
    size_t const uncompressed_end,
    rmf_match match)
{
    size_t len_end = enc->len_end_max;
    unsigned const search_depth = tbl->depth;
    do {
        size_t const pos_mask = enc->pos_mask;

        /* Reset all prices that were set last time */
        for (; (len_end & 3) != 0; --len_end)
            enc->opt_buf[len_end].price = RC_INFINITY_PRICE;
        for (; len_end >= 4; len_end -= 4) {
            enc->opt_buf[len_end].price = RC_INFINITY_PRICE;
            enc->opt_buf[len_end - 1].price = RC_INFINITY_PRICE;
            enc->opt_buf[len_end - 2].price = RC_INFINITY_PRICE;
            enc->opt_buf[len_end - 3].price = RC_INFINITY_PRICE;
        }

        /* Set everything up at position 0 */
        size_t pos = start_index;
        uint32_t reps[REPS];
        len_end = lzma_init_optimizer_pos0(enc, block, match, pos, is_hybrid, reps);
        match.length = 0;
        size_t cur = 1;

        /* len_end == 0 if a match of fast_length was found */
        if (len_end > 0) {
            ++pos;
            for (; cur < len_end; ++cur, ++pos) {
                /* Terminate if the farthest calculated price is too near the buffer end */
                if (len_end >= OPT_BUF_SIZE - OPT_END_SIZE) {
                    uint32_t price = enc->opt_buf[cur].price;
                    /* This is a compromise to favor more distant end points
                     * even if the price is a bit higher */
                    uint32_t const delta = price / (uint32_t)cur / 2U;
                    for (size_t j = cur + 1; j <= len_end; j++) {
                        uint32_t const price2 = enc->opt_buf[j].price;
                        if (price >= price2) {
                            price = price2;
                            cur = j;
                        }
                        price += delta;
                    }
                    break;
                }

                /* Skip ahead if a lower or equal price is available at greater distance */
                size_t const end = my_min(cur + OPT_SKIP_SIZE, len_end);
                uint32_t price = enc->opt_buf[cur].price;
                for (size_t j = cur + 1; j <= end; j++) {
                    uint32_t const price2 = enc->opt_buf[j].price;
                    if (price >= price2) {
                        price = price2;
                        pos += j - cur;
                        cur = j;
                        if (cur == len_end)
                            goto reverse;
                    }
                }

                match = rmf_get_match(block, tbl, search_depth, struct_tbl, pos);
                if (match.length >= enc->fast_length)
                    break;

                len_end = lzma_optimal_parse(enc, block, match, pos, cur, len_end, is_hybrid, reps);
            }
reverse:
            DEBUGLOG(6, "End optimal parse at %u", (uint32_t)cur);
            lzma_reverse_optimal_chain(enc->opt_buf, cur);
        }
        /* Encode the selections in the buffer */
        size_t i = 0;
        do {
            unsigned const len = enc->opt_buf[i].len;

            if (len == 1 && enc->opt_buf[i].dist == NULL_DIST) {
                lzma_encode_literal_buf(enc, block.data, start_index + i);
                ++i;
            }
            else {
                size_t const pos_state = (start_index + i) & pos_mask;
                uint32_t const dist = enc->opt_buf[i].dist;
                /* Updating i separately for each case may allow a branch to be eliminated */
                if (dist >= REPS) {
                    lzma_encode_normal_match(enc, len, dist - REPS, pos_state);
                    i += len;
                }
                else if(len == 1) {
                    lzma_encode_rep_short(enc, pos_state);
                    ++i;
                }
                else {
                    lzma_encode_rep_long(enc, len, dist, pos_state);
                    i += len;
                }
            }
        } while (i < cur);
        start_index += i;
        /* Do another round if there is a long match pending,
         * because the reps must be checked and the match encoded. */
    } while (match.length >= enc->fast_length
		&& start_index < uncompressed_end
		&& rc_chunk_size(&enc->rc) < enc->chunk_size);

    enc->len_end_max = len_end;

    return start_index;
}

static void FORCE_NOINLINE lzma_fill_align_prices(lzma2_rmf_encoder *const enc)
{
    unsigned i;
    const probability *const probs = enc->states.dist_align_encoders;
    for (i = 0; i < ALIGN_SIZE / 2; i++) {
        uint32_t price = 0;
        unsigned sym = i;
        unsigned m = 1;
        unsigned bit;
        bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[m], bit); m = (m << 1) + bit;
        bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[m], bit); m = (m << 1) + bit;
        bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[m], bit); m = (m << 1) + bit;
        uint32_t const prob = probs[m];
        enc->align_prices[i] = price + GET_PRICE_0(prob);
        enc->align_prices[i + 8] = price + GET_PRICE_1(prob);
    }
}

static void FORCE_NOINLINE lzma_fill_dist_prices(lzma2_rmf_encoder *const enc)
{
    uint32_t * const temp_prices = enc->distance_prices[DIST_STATES - 1];

    enc->match_price_count = 0;

    for (size_t i = DIST_MODEL_START / 2; i < FULL_DISTANCES / 2; i++) {
        unsigned const dist_slot = lzma_fastpos[i];
        unsigned footer_bits = (dist_slot >> 1) - 1;
        size_t base = ((2 | (dist_slot & 1)) << footer_bits);
        const probability *probs = enc->states.dist_encoders + base * 2U;
        base += i;
        probs = probs - lzma_fastpos[base] - 1;
        uint32_t price = 0;
        unsigned m = 1;
        unsigned sym = (unsigned)i;
        unsigned const offset = (unsigned)1 << footer_bits;

        for (; footer_bits != 0; --footer_bits) {
            unsigned bit = sym & 1;
            sym >>= 1;
            price += GET_PRICE(probs[m], bit);
            m = (m << 1) + bit;
        };

        unsigned const prob = probs[m];
        temp_prices[base] = price + GET_PRICE_0(prob);
        temp_prices[base + offset] = price + GET_PRICE_1(prob);
    }

    for (unsigned lps = 0; lps < DIST_STATES; lps++) {
        size_t slot;
        size_t const dist_table_size2 = (enc->dist_price_table_size + 1) >> 1;
        uint32_t *const dist_slot_prices = enc->dist_slot_prices[lps];
        const probability *const probs = enc->states.dist_slot_encoders[lps];

        for (slot = 0; slot < dist_table_size2; slot++) {
            /* dist_slot_prices[slot] = RcTree_GetPrice(encoder, DIST_SLOT_BITS, slot, p->ProbPrices); */
            uint32_t price;
            unsigned bit;
            unsigned sym = (unsigned)slot + (1 << (DIST_SLOT_BITS - 1));
            bit = sym & 1; sym >>= 1; price = GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            bit = sym & 1; sym >>= 1; price += GET_PRICE(probs[sym], bit);
            unsigned const prob = probs[slot + (1 << (DIST_SLOT_BITS - 1))];
            dist_slot_prices[slot * 2] = price + GET_PRICE_0(prob);
            dist_slot_prices[slot * 2 + 1] = price + GET_PRICE_1(prob);
        }

        {
            uint32_t delta = ((uint32_t)((DIST_MODEL_END / 2 - 1) - ALIGN_BITS) << RC_BIT_PRICE_SHIFT_BITS);
            for (slot = DIST_MODEL_END / 2; slot < dist_table_size2; slot++) {
                dist_slot_prices[slot * 2] += delta;
                dist_slot_prices[slot * 2 + 1] += delta;
                delta += ((uint32_t)1 << RC_BIT_PRICE_SHIFT_BITS);
            }
        }

        {
            uint32_t *const dp = enc->distance_prices[lps];

            dp[0] = dist_slot_prices[0];
            dp[1] = dist_slot_prices[1];
            dp[2] = dist_slot_prices[2];
            dp[3] = dist_slot_prices[3];

            for (size_t i = 4; i < FULL_DISTANCES; i += 2) {
                uint32_t slot_price = dist_slot_prices[lzma_fastpos[i]];
                dp[i] = slot_price + temp_prices[i];
                dp[i + 1] = slot_price + temp_prices[i + 1];
            }
        }
    }
}

FORCE_INLINE_TEMPLATE
size_t lzma_encode_chunk_best(lzma2_rmf_encoder *const enc,
    lzma_data_block const block,
    rmf_match_table* const tbl,
    int const struct_tbl,
    size_t pos,
    size_t const uncompressed_end)
{
    unsigned const search_depth = tbl->depth;
    lzma_fill_dist_prices(enc);
    lzma_fill_align_prices(enc);
    lzma_len_update_prices(enc, &enc->states.len_states);
    lzma_len_update_prices(enc, &enc->states.rep_len_states);

    while (pos < uncompressed_end && enc->rc.out_index < enc->chunk_size)
    {
        rmf_match const match = rmf_get_match(block, tbl, search_depth, struct_tbl, pos);
        if (match.length > 1) {
            /* Template-like inline function */
            if (enc->strategy == LZMA_MODE_ULTRA) {
                pos = lzma_encode_opt_sequence(enc, block, tbl, struct_tbl, 1, pos, uncompressed_end, match);
            }
            else {
                pos = lzma_encode_opt_sequence(enc, block, tbl, struct_tbl, 0, pos, uncompressed_end, match);
            }
            if (enc->match_price_count >= MATCH_REPRICE_FREQ) {
                lzma_fill_align_prices(enc);
                lzma_fill_dist_prices(enc);
                lzma_len_update_prices(enc, &enc->states.len_states);
            }
            if (enc->rep_len_price_count >= REP_LEN_REPRICE_FREQ) {
                enc->rep_len_price_count = 0;
                lzma_len_update_prices(enc, &enc->states.rep_len_states);
            }
        }
        else {
            if (block.data[pos] != block.data[pos - enc->states.reps[0] - 1]) {
                lzma_encode_literal_buf(enc, block.data, pos);
                ++pos;
            }
            else {
                lzma_encode_rep_short(enc, pos & enc->pos_mask);
                ++pos;
            }
        }
    }
    return pos;
}

static void lzma_len_probs_reset(lzma2_len_states* const ls, unsigned const fast_length)
{
    ls->choice = RC_PROB_INIT_VALUE;

    for (size_t i = 0; i < (POS_STATES_MAX << (LEN_LOW_BITS + 1)); ++i)
        ls->low[i] = RC_PROB_INIT_VALUE;

    for (size_t i = 0; i < LEN_HIGH_SYMBOLS; ++i)
        ls->high[i] = RC_PROB_INIT_VALUE;

    ls->table_size = fast_length + 1 - MATCH_LEN_MIN;
}

static void lzma_probs_reset(lzma2_enc_states* const es, unsigned const lc, unsigned const lp, unsigned fast_length)
{
    es->state = 0;

    for (size_t i = 0; i < REPS; ++i)
        es->reps[i] = 0;

    for (size_t i = 0; i < STATES; ++i) {
        for (size_t j = 0; j < POS_STATES_MAX; ++j) {
            es->is_match[i][j] = RC_PROB_INIT_VALUE;
            es->is_rep0_long[i][j] = RC_PROB_INIT_VALUE;
        }
        es->is_rep[i] = RC_PROB_INIT_VALUE;
        es->is_rep_G0[i] = RC_PROB_INIT_VALUE;
        es->is_rep_G1[i] = RC_PROB_INIT_VALUE;
        es->is_rep_G2[i] = RC_PROB_INIT_VALUE;
    }
    size_t const num = (size_t)LITERAL_CODER_SIZE << (lp + lc);
    for (size_t i = 0; i < num; ++i)
        es->literal_probs[i] = RC_PROB_INIT_VALUE;

    for (size_t i = 0; i < DIST_STATES; ++i) {
        probability *probs = es->dist_slot_encoders[i];
        for (size_t j = 0; j < (1 << DIST_SLOT_BITS); ++j)
            probs[j] = RC_PROB_INIT_VALUE;
    }
    for (size_t i = 0; i < FULL_DISTANCES - DIST_MODEL_END; ++i)
        es->dist_encoders[i] = RC_PROB_INIT_VALUE;

    lzma_len_probs_reset(&es->len_states, fast_length);
    lzma_len_probs_reset(&es->rep_len_states, fast_length);

    for (size_t i = 0; i < (1 << ALIGN_BITS); ++i)
        es->dist_align_encoders[i] = RC_PROB_INIT_VALUE;
}

size_t lzma2_enc_rmf_mem_usage(unsigned const chain_log, lzma_mode const strategy, unsigned const thread_count)
{
    size_t size = sizeof(lzma2_rmf_encoder);
    if(strategy == LZMA_MODE_ULTRA)
        size += sizeof(lzma2_hc3) + (sizeof(uint32_t) << chain_log) - sizeof(uint32_t);
    return size * thread_count;
}

static void lzma2_reset(lzma2_rmf_encoder *const enc, size_t const max_distance)
{
    DEBUGLOG(5, "LZMA encoder reset : max_distance %u", (unsigned)max_distance);
    rcf_reset(&enc->rc);
    lzma_probs_reset(&enc->states, enc->lc, enc->lp, enc->fast_length);
    enc->pos_mask = (1 << enc->pb) - 1;
    enc->lit_pos_mask = ((size_t)0x100 << enc->lp) - ((size_t)0x100 >> enc->lc);
	uint32_t i = 0;
    for (; max_distance > (size_t)1 << i; ++i) {
    }
    enc->dist_price_table_size = i * 2;
    enc->rep_len_price_count = 0;
    enc->match_price_count = 0;
}

/* Integer square root from https://stackoverflow.com/a/1101217 */
static uint32_t lzma2_isqrt(uint32_t op)
{
    uint32_t res = 0;
    /* "one" starts at the highest power of four <= than the argument. */
    uint32_t one = (uint32_t)1 << (bsr32(op) & ~1);

    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = res + 2U * one;
        }
        res >>= 1;
        one >>= 2;
    }
    return res;
}

static uint8_t lzma2_is_chunk_incompressible(const rmf_match_table* const tbl,
    lzma_data_block const block, size_t const start,
	unsigned const strategy)
{
	if (block.end - start >= TEST_MIN_CHUNK_SIZE) {
		static const size_t max_dist_table[][5] = {
			{ 0, 0, 0, 1U << 6, 1U << 14 }, /* fast */
			{ 0, 0, 1U << 6, 1U << 14, 1U << 22 }, /* opt */
			{ 0, 0, 1U << 6, 1U << 14, 1U << 22 } }; /* ultra */
		static const size_t margin_divisor[3] = { 60U, 45U, 120U };
		static const uint32_t dev_table[3] = { 24, 24, 20};

		size_t const end = my_min(start + CHUNK_SIZE, block.end);
		size_t const chunk_size = end - start;
		size_t count = 0;
		size_t const margin = chunk_size / margin_divisor[strategy];
		size_t const terminator = start + margin;

		if (tbl->is_struct) {
			size_t prev_dist = 0;
			for (size_t pos = start; pos < end; ) {
				uint32_t const link = GetMatchLink(tbl->table, pos);
				if (link == RADIX_NULL_LINK) {
					++pos;
					++count;
					prev_dist = 0;
				}
				else {
					size_t const length = GetMatchLength(tbl->table, pos);
					size_t const dist = pos - GetMatchLink(tbl->table, pos);
                    if (length > 4) {
                        /* Increase the cost if it's not the same match */
                        count += dist != prev_dist;
                    }
                    else {
                        /* Increment the cost for a short match. The cost is the entire length if it's too far */
                        count += (dist < max_dist_table[strategy][length]) ? 1 : length;
                    }
					pos += length;
					prev_dist = dist;
				}
				if (count + terminator <= pos)
					return 0;
			}
		}
		else {
			size_t prev_dist = 0;
			for (size_t pos = start; pos < end; ) {
				uint32_t const link = tbl->table[pos];
				if (link == RADIX_NULL_LINK) {
					++pos;
					++count;
					prev_dist = 0;
				}
				else {
					size_t const length = link >> RADIX_LINK_BITS;
					size_t const dist = pos - (link & RADIX_LINK_MASK);
					if (length > 4)
						count += dist != prev_dist;
					else
						count += (dist < max_dist_table[strategy][length]) ? 1 : length;
					pos += length;
					prev_dist = dist;
				}
				if (count + terminator <= pos)
					return 0;
			}
		}

        uint32_t char_count[256];
        uint32_t char_total = 0;
        /* Expected normal character count * 4 */
        uint32_t const avg = (uint32_t)(chunk_size / 64U);

        memset(char_count, 0, sizeof(char_count));
        for (size_t pos = start; pos < end; ++pos)
            char_count[block.data[pos]] += 4;
        /* Sum the deviations */
        for (size_t i = 0; i < 256; ++i) {
            int32_t delta = char_count[i] - avg;
            char_total += delta * delta;
        }
        uint32_t sqrt_chunk = (chunk_size == CHUNK_SIZE) ? SQRT_CHUNK_SIZE : lzma2_isqrt((uint32_t)chunk_size);
        /* Result base on character count std dev */
        return lzma2_isqrt(char_total) / sqrt_chunk <= dev_table[strategy];
	}
	return 0;
}

static size_t lzma2_encode_chunk(lzma2_rmf_encoder *const enc,
    rmf_match_table* const tbl,
    lzma_data_block const block,
    size_t const pos, size_t const uncompressed_end)
{
    /* Template-like inline functions */
    if (enc->strategy == LZMA_MODE_FAST) {
        if (tbl->is_struct) {
            return lzma_encode_chunk_fast(enc, block, tbl, 1,
                pos, uncompressed_end);
        }
        else {
            return lzma_encode_chunk_fast(enc, block, tbl, 0,
                pos, uncompressed_end);
        }
    }
    else {
        if (tbl->is_struct) {
            return lzma_encode_chunk_best(enc, block, tbl, 1,
                pos, uncompressed_end);
        }
        else {
            return lzma_encode_chunk_best(enc, block, tbl, 0,
                pos, uncompressed_end);
        }
    }
}

size_t lzma2_rmf_encode(lzma2_rmf_encoder *const enc,
	rmf_match_table* const tbl,
	lzma_data_block const block,
	const lzma_options_lzma* const options,
	FL2_atomic *const progress_in,
	FL2_atomic *const progress_out,
	bool *const canceled)
{
    size_t const start = block.start;

    /* Output starts in the temp buffer */
    uint8_t* out_dest = enc->out_buf;
    enc->chunk_size = TEMP_MIN_OUTPUT;
    enc->chunk_limit = TEMP_BUFFER_SIZE - MATCH_MAX_OUT_SIZE * 2;

    /* Each encoder writes a properties byte because the upstream encoder(s) could */
	/* write only uncompressed chunks with no properties. */
	uint8_t encode_properties = 1;
    uint8_t incompressible = 0;

	assert(block.end > block.start);
	if (block.end <= block.start)
        return 0;

    enc->lc = options->lc;
    enc->lp = options->lp;
    enc->pb = options->pb;
    enc->strategy = options->mode;
    enc->fast_length = my_min(options->nice_len, MATCH_LEN_MAX);
    enc->match_cycles = my_min(options->near_depth, MATCHES_MAX - 1);

    lzma2_reset(enc, block.end);

    if (enc->strategy == LZMA_MODE_ULTRA) {
        lzma_hash_reset(enc, options->near_dict_size_log);
        enc->hash_prev_index = (start >= (size_t)enc->hash_dict_3) ? (ptrdiff_t)(start - enc->hash_dict_3) : (ptrdiff_t)-1;
    }
    enc->len_end_max = OPT_BUF_SIZE - 1;

    /* Limit the matches near the end of this slice to not exceed block.end */
    rmf_limit_lengths(tbl, block.end);

    for (size_t pos = start; pos < block.end;) {
        size_t header_size = encode_properties ? CHUNK_HEADER_SIZE + 1 : CHUNK_HEADER_SIZE;
        lzma2_enc_states saved_states;
        size_t next_index;

        rcf_reset(&enc->rc);
        rcf_set_output_buffer(&enc->rc, out_dest + header_size);

        if (!incompressible) {
            size_t cur = pos;
            size_t const end = (enc->strategy == LZMA_MODE_FAST) ? my_min(block.end, pos + CHUNK_UNCOMPRESSED_MAX - MATCH_LEN_MAX + 1)
                : my_min(block.end, pos + CHUNK_UNCOMPRESSED_MAX - OPT_BUF_SIZE + 2); /* last byte of opt_buf unused */

            /* Copy states in case chunk is incompressible */
            saved_states = enc->states;

            if (pos == 0) {
                /* First byte of the dictionary */
                lzma_encode_literal(enc, 0, block.data[0], 0);
                ++cur;
            }
            if (pos == start) {
                /* After TEMP_MIN_OUTPUT bytes we can write data to the match table because the */
                /* compressed data will never catch up with the table position being read. */
                cur = lzma2_encode_chunk(enc, tbl, block, cur, end);

				if (header_size + enc->rc.out_index > TEMP_BUFFER_SIZE)
					return (size_t)-1;

                /* Switch to the match table as output buffer */
                out_dest = rmf_output_buffer(tbl, start);
                memcpy(out_dest, enc->out_buf, header_size + enc->rc.out_index);
                enc->rc.out_buffer = out_dest + header_size;

                /* Now encode up to the full chunk size */
                enc->chunk_size = CHUNK_SIZE;
                enc->chunk_limit = CHUNK_COMPRESSED_MAX - MATCH_MAX_OUT_SIZE * 2;
            }
            next_index = lzma2_encode_chunk(enc, tbl, block, cur, end);
            rcf_flush(&enc->rc);
        }
        else {
            next_index = my_min(pos + CHUNK_SIZE, block.end);
        }
        size_t compressed_size = enc->rc.out_index;
        size_t uncompressed_size = next_index - pos;

        if (compressed_size > CHUNK_COMPRESSED_MAX || uncompressed_size > CHUNK_UNCOMPRESSED_MAX)
            return (size_t)-1;

        uint8_t* header = out_dest;

        header[1] = (uint8_t)((uncompressed_size - 1) >> 8);
        header[2] = (uint8_t)(uncompressed_size - 1);
        /* Output an uncompressed chunk if necessary */
        if (incompressible || uncompressed_size + 3 <= compressed_size + header_size) {
            DEBUGLOG(6, "Storing chunk : was %u => %u", (unsigned)uncompressed_size, (unsigned)compressed_size);

            header[0] = (pos == 0) ? CHUNK_UNCOMP_DICT_RESET : CHUNK_UNCOMPRESSED;

            /* Copy uncompressed data into the output */
            memcpy(header + 3, block.data + pos, uncompressed_size);

            compressed_size = uncompressed_size;
            header_size = 3 + (header - out_dest);

            /* Restore states if compression was attempted */
            if (!incompressible)
                enc->states = saved_states;
        }
        else {
            DEBUGLOG(6, "Compressed chunk : %u => %u", (unsigned)uncompressed_size, (unsigned)compressed_size);

            if (pos == 0)
                header[0] = CHUNK_COMPRESSED_FLAG | CHUNK_ALL_RESET;
            else if (encode_properties)
                header[0] = CHUNK_COMPRESSED_FLAG | CHUNK_STATE_PROP_RESET;
            else
                header[0] = CHUNK_COMPRESSED_FLAG | CHUNK_NOTHING_RESET;

            header[0] |= (uint8_t)((uncompressed_size - 1) >> 16);
            header[3] = (uint8_t)((compressed_size - 1) >> 8);
            header[4] = (uint8_t)(compressed_size - 1);
            if (encode_properties) {
                if(lzma_lzma_lclppb_encode(options, header + 5))
					return (size_t)-1;
				encode_properties = 0;
            }
        }
        if (incompressible || uncompressed_size + 3 <= compressed_size + (compressed_size >> RANDOM_FILTER_MARGIN_BITS) + header_size) {
            /* Test the next chunk for compressibility */
            incompressible = lzma2_is_chunk_incompressible(tbl, block, next_index, enc->strategy - 1);
        }
        out_dest += compressed_size + header_size;

        /* Update progress concurrently with other encoder threads */
        FL2_atomic_add(*progress_in, (long)(next_index - pos));
        FL2_atomic_add(*progress_out, (long)(compressed_size + header_size));

        pos = next_index;

        if (*canceled)
            return 0;
    }
    return out_dest - rmf_output_buffer(tbl, start);
}
