///////////////////////////////////////////////////////////////////////////////
//
/// \file       range_fast_enc.c
/// \brief      Range encoder for fast LZMA2
///
//  Authors:    Igor Pavlov
//              Conor McCarthy
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "range_fast_enc.h"


void
rcf_set_output_buffer(lzma_range_fast_enc* const rc, uint8_t *const out_buffer)
{
	rc->out_buffer = out_buffer;
	rc->out_index = 0;
}

void
rcf_reset(lzma_range_fast_enc* const rc)
{
	rc->low = 0;
	rc->range = (uint32_t)-1;
	rc->cache_size = 0;
	rc->cache = 0;
}


#if defined(__x86_64__) || defined(_M_X64) || SIZEOF_SIZE_T >= 8

void force_noinline
rcf_shift_low(lzma_range_fast_enc* const rc)
{
	uint64_t low = rc->low;
	rc->low = (uint32_t)(low << 8);
	// VC15 compiles 'if (low < 0xFF000000 || low > 0xFFFFFFFF)' to this single-branch conditional
	if (low + 0xFFFFFFFF01000000 > 0xFFFFFF) {
		uint8_t high = (uint8_t)(low >> 32);
		rc->out_buffer[rc->out_index++] = rc->cache + high;
		rc->cache = (uint8_t)(low >> 24);
		if (rc->cache_size != 0) {
			high += 0xFF;
			do {
				rc->out_buffer[rc->out_index++] = high;
			} while (--rc->cache_size != 0);
		}
	}
	else {
		rc->cache_size++;
	}
}

#else

void force_noinline
rcf_shift_low(lzma_range_fast_enc* const rc)
{
	uint32_t low = (uint32_t)rc->low;
	unsigned high = (unsigned)(rc->low >> 32);
	rc->low = low << 8;
	if (low < (uint32_t)0xFF000000 || high != 0) {
		rc->out_buffer[rc->out_index++] = rc->cache + (uint8_t)high;
		rc->cache = (uint8_t)(low >> 24);
		if (rc->cache_size != 0) {
			high += 0xFF;
			do {
				rc->out_buffer[rc->out_index++] = (uint8_t)high;
			} while (--rc->cache_size != 0);
		}
	}
	else {
		rc->cache_size++;
	}
}

#endif


void
rcf_bittree(lzma_range_fast_enc* const rc, probability *const probs, unsigned bit_count, unsigned symbol)
{
	assert(bit_count > 1);
	--bit_count;
	unsigned bit = symbol >> bit_count;
	rcf_bit(rc, &probs[1], bit);
	size_t tree_index = 1;
	do {
		--bit_count;
		tree_index = (tree_index << 1) | bit;
		bit = (symbol >> bit_count) & 1;
		rcf_bit(rc, &probs[tree_index], bit);
	} while (bit_count != 0);
}


void
rcf_bittree_reverse(lzma_range_fast_enc* const rc, probability *const probs, unsigned bit_count, unsigned symbol)
{
	assert(bit_count != 0);
	unsigned bit = symbol & 1;
	rcf_bit(rc, &probs[1], bit);
	unsigned tree_index = 1;
	while (--bit_count != 0) {
		tree_index = (tree_index << 1) + bit;
		symbol >>= 1;
		bit = symbol & 1;
		rcf_bit(rc, &probs[tree_index], bit);
	}
}


void force_noinline
rcf_direct(lzma_range_fast_enc* const rc, unsigned value, unsigned bit_count)
{
	assert(bit_count > 0);
	do {
		rc->range >>= 1;
		--bit_count;
		rc->low += rc->range & -((int)(value >> bit_count) & 1);
		if (rc->range < RC_TOP_VALUE) {
			rc->range <<= 8;
			rcf_shift_low(rc);
		}
	} while (bit_count != 0);
}


