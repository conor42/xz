///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma2_encoder_rmf.h
/// \brief      LZMA2 encoder for radix match-finder
///
//  Authors:    Igor Pavlov
//              Conor McCarthy
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#ifndef LZMA_LZMA2_ENCODER_RMF_H
#define LZMA_LZMA2_ENCODER_RMF_H

#include "lzma_encoder_private.h"
#include "data_block.h"
#include "radix_mf.h"
#include "range_fast_enc.h"
#include "atomic.h"

#define NEAR_DICT_LOG_MIN 4U
#define NEAR_DICT_LOG_MAX 14U
#define MATCH_CYCLES_MAX 64U

// Enough for 8 threads, 1 Mb dict, 2/16 overlap
#define ENC_MIN_BYTES_PER_THREAD 0x1C000

#define LZMA2_END_MARKER '\0'

#define MATCH_REPRICE_FREQ 64U
#define REP_LEN_REPRICE_FREQ 64U

#define MATCHES_MAX (MATCH_CYCLES_MAX + 1)

#define OPT_END_SIZE 32U
#define OPT_BUF_SIZE (MATCH_LEN_MAX * 2U + OPT_END_SIZE)
#define OPT_SKIP_SIZE 16U

#define HC3_BITS 14U

// Hard to define where the match table read pos definitely catches up with the output size, but
// 64 bytes of input expanding beyond 256 bytes right after an encoder reset is most likely impossible.
// The encoder will error out if that happens.
#define TEMP_MIN_OUTPUT 256U
#define TEMP_BUFFER_SIZE (TEMP_MIN_OUTPUT + OPT_BUF_SIZE + OPT_BUF_SIZE / 4U)


// Probabilities and prices for encoding match lengths.
// Two objects of this type are needed, one for normal matches
// and another for rep matches.
typedef struct 
{
	size_t table_size;
	unsigned prices[POS_STATES_MAX][LEN_SYMBOLS];
	probability choice; // low[0] is choice_2. Must be consecutive for speed.
	probability low[POS_STATES_MAX << (LEN_LOW_BITS + 1)];
	probability high[LEN_HIGH_SYMBOLS];
} lzma2_len_states;


// All probabilities for the encoder. This is separate from the encoder object
// so the state can be saved and restored in case a chunk is not compressible.
typedef struct
{
	// Fields are ordered for speed.
	lzma2_len_states rep_len_states;
	probability is_rep0_long[STATES][POS_STATES_MAX];

	size_t state;
	uint32_t reps[REPS];

	probability is_match[STATES][POS_STATES_MAX];
	probability is_rep[STATES];
	probability is_rep_G0[STATES];
	probability is_rep_G1[STATES];
	probability is_rep_G2[STATES];

	lzma2_len_states len_states;

	probability dist_slot_encoders[DIST_STATES][DIST_SLOTS];
	probability dist_align_encoders[ALIGN_SIZE];
	probability dist_encoders[FULL_DISTANCES - DIST_MODEL_END];

	probability literal_probs[LITERAL_CODER_SIZE << LZMA_LCLP_MAX];
} lzma2_enc_states;


// Linked list item for optimal parsing
typedef struct
{
	size_t state;
	uint32_t price;
	// extra = 0 : normal
	//         1 : LIT, MATCH
	//        >1 : MATCH (extra-1), LIT, REP0 (len)
	unsigned extra;
	unsigned len;
	uint32_t dist;
	uint32_t reps[REPS];
} lzma2_node;


// Table and chain for 3-byte hash. Extra elements in hash_chain_3 are malloc'd.
typedef struct {
	int32_t table_3[1 << HC3_BITS];
	int32_t hash_chain_3[1];
} lzma2_hc3;


typedef struct
{
	unsigned lc;
	unsigned lp;
	unsigned pb;
	unsigned fast_length;
	size_t len_end_max;
	size_t lit_pos_mask;
	size_t pos_mask;
	unsigned match_cycles;
	lzma_mode strategy;

	lzma_range_fast_enc rc;
	// Finish writing the chunk at this size.
	size_t chunk_size;
	// Don't encode a symbol beyond this limit (used by fast mode).
	size_t chunk_limit;

	lzma2_enc_states states;

	unsigned match_price_count;
	unsigned rep_len_price_count;
	size_t dist_price_table_size;
	unsigned align_prices[ALIGN_SIZE];
	unsigned dist_slot_prices[DIST_STATES][DIST_SLOTS];
	unsigned distance_prices[DIST_STATES][FULL_DISTANCES];

	// base_match allows access to matches[-1] in LZMA_optimalParse.
	rmf_match base_match;
	rmf_match matches[MATCHES_MAX];
	size_t match_count;

	lzma2_node opt_buf[OPT_BUF_SIZE];

	lzma2_hc3* hash_buf;
	ptrdiff_t chain_mask_3;
	ptrdiff_t hash_dict_3;
	ptrdiff_t hash_prev_index;
	ptrdiff_t hash_alloc_3;

	// Temp output buffer used before space frees up in the match table.
	uint8_t out_buf[TEMP_BUFFER_SIZE];
} lzma2_rmf_encoder;

extern void lzma2_rmf_enc_construct(lzma2_rmf_encoder *const enc);

extern void lzma2_rmf_enc_free(lzma2_rmf_encoder *const enc);

extern int lzma2_rmf_hash_alloc(lzma2_rmf_encoder *const enc, const lzma_options_lzma* const options);

extern size_t lzma2_rmf_encode(lzma2_rmf_encoder *const restrict enc,
		rmf_match_table *const restrict tbl,
		lzma_data_block const block,
		const lzma_options_lzma *const options,
		lzma_atomic *const progress_in,
		lzma_atomic *const progress_out,
		bool *const canceled);

extern size_t lzma2_enc_rmf_mem_usage(unsigned const chain_log,
		lzma_mode const strategy, unsigned const thread_count);


#endif // LZMA_LZMA2_ENCODER_RMF_H