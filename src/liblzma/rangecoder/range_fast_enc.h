///////////////////////////////////////////////////////////////////////////////
//
/// \file       range_fast_enc.h
/// \brief      Range encoder for fast LZMA2
///
//  Authors:    Igor Pavlov
//              Conor McCarthy
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef RANGE_FAST_ENC_H
#define RANGE_FAST_ENC_H

#include "range_common.h"


#define RC_PROB_INIT_VALUE (RC_BIT_MODEL_TOTAL >> 1U)
#define RC_PRICE_TABLE_SIZE (RC_BIT_MODEL_TOTAL >> RC_MOVE_REDUCING_BITS)

#define MIN_LITERAL_PRICE 8U


// This range encoder cannot be used with lzma_encoder because cache_size is not uint64_t.
typedef struct
{
	uint8_t *out_buffer;
	size_t out_index;
	size_t cache_size;
	uint64_t low;
	uint32_t range;
	uint8_t cache;
} lzma_range_fast_enc;

void rcf_reset(lzma_range_fast_enc *const rc);

void rcf_set_output_buffer(lzma_range_fast_enc *const rc, uint8_t *const out_buffer);

void force_noinline rcf_shift_low(lzma_range_fast_enc *const restrict rc);

void rcf_bittree(lzma_range_fast_enc *const restrict rc, probability *const restrict probs, unsigned bit_count, unsigned symbol);

void rcf_bittree_reverse(lzma_range_fast_enc *const restrict rc, probability *const restrict probs, unsigned bit_count, unsigned symbol);

void force_noinline rcf_direct(lzma_range_fast_enc *const restrict rc, unsigned value, unsigned bit_count);


static hint_inline void
rcf_bit_0(lzma_range_fast_enc *const restrict rc, probability *const restrict rprob)
{
	unsigned prob = *rprob;
	rc->range = (rc->range >> RC_BIT_MODEL_TOTAL_BITS) * prob;
	prob += (RC_BIT_MODEL_TOTAL - prob) >> RC_MOVE_BITS;
	*rprob = (probability)prob;
	if (rc->range < RC_TOP_VALUE) {
		rc->range <<= 8;
		rcf_shift_low(rc);
	}
}


static hint_inline void
rcf_bit_1(lzma_range_fast_enc *const restrict rc, probability *const restrict rprob)
{
	unsigned prob = *rprob;
	uint32_t new_bound = (rc->range >> RC_BIT_MODEL_TOTAL_BITS) * prob;
	rc->low += new_bound;
	rc->range -= new_bound;
	prob -= prob >> RC_MOVE_BITS;
	*rprob = (probability)prob;
	if (rc->range < RC_TOP_VALUE) {
		rc->range <<= 8;
		rcf_shift_low(rc);
	}
}


static hint_inline void
rcf_bit(lzma_range_fast_enc *const restrict rc, probability *const restrict rprob, unsigned const bit)
{
	unsigned prob = *rprob;
	if (bit != 0) {
		uint32_t const new_bound = (rc->range >> RC_BIT_MODEL_TOTAL_BITS) * prob;
		rc->low += new_bound;
		rc->range -= new_bound;
		prob -= prob >> RC_MOVE_BITS;
	}
	else {
		rc->range = (rc->range >> RC_BIT_MODEL_TOTAL_BITS) * prob;
		prob += (RC_BIT_MODEL_TOTAL - prob) >> RC_MOVE_BITS;
	}
	*rprob = (probability)prob;
	if (rc->range < RC_TOP_VALUE) {
		rc->range <<= 8;
		rcf_shift_low(rc);
	}
}


static hint_inline void
rcf_flush(lzma_range_fast_enc *const rc)
{
	for (int i = 0; i < 5; ++i)
		rcf_shift_low(rc);
}

static hint_inline size_t
rcf_chunk_size(const lzma_range_fast_enc *const restrict rc)
{
	return rc->out_index + rc->cache_size + 5 - 1;
}

#endif // RANGE_FAST_ENC_H
