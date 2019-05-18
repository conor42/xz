/* lzma2_enc.h -- LZMA2 Encoder
Based on LzmaEnc.h and Lzma2Enc.h : Igor Pavlov
Modified for FL2 by Conor McCarthy
Public domain
*/

#ifndef RADYX_LZMA2_ENCODER_H
#define RADYX_LZMA2_ENCODER_H

#include "lzma_encoder_private.h"
#include "data_block.h"
#include "radix_mf.h"
#include "range_enc.h"
#include "atomic.h"

#if defined (__cplusplus)
extern "C" {
#endif

#define NEAR_DICT_LOG_MIN 4
#define NEAR_DICT_LOG_MAX 14
#define MATCH_CYCLES_MAX 64

#define kFastDistBits 12U

#define LZMA2_END_MARKER '\0'
#define ENC_MIN_BYTES_PER_THREAD 0x1C000 /* Enough for 8 threads, 1 Mb dict, 2/16 overlap */

#define kNumReps 4U

#define kNumLiterals 0x100U
#define kNumLitTables 3U

#define kDicLogSizeMin 18U
#define kDicLogSizeMax 31U
#define kDistTableSizeMax (kDicLogSizeMax * 2U)

#define kMatchRepriceFrequency 64U
#define kRepLenRepriceFrequency 64U

#define kMatchesMax 65U /* Doesn't need to be larger than FL2_HYBRIDCYCLES_MAX + 1 */

#define kOptimizerEndSize 32U
#define kOptimizerBufferSize (MATCH_LEN_MAX * 2U + kOptimizerEndSize)
#define kOptimizerSkipSize 16U
#define kInfinityPrice (1UL << 30U)
#define kNullDist (uint32_t)-1

#define kMaxMatchEncodeSize 20

#define kMaxChunkCompressedSize (1UL << 16U)
	/* Need to leave sufficient space for expanded output from a full opt buffer with bad starting probs */
#define kChunkSize (kMaxChunkCompressedSize - 2048U)
#define kSqrtChunkSize 252U

/* Hard to define where the match table read pos definitely catches up with the output size, but
 * 64 bytes of input expanding beyond 256 bytes right after an encoder reset is most likely impossible.
 * The encoder will error out if this happens. */
#define kTempMinOutput 256U
#define kTempBufferSize (kTempMinOutput + kOptimizerBufferSize + kOptimizerBufferSize / 4U)

#define kMaxHashDictBits 14U
#define kHash3Bits 14U
#define kNullLink -1

 /* Probabilities and prices for encoding match lengths.
 * Two objects of this type are needed, one for normal matches
 * and another for rep matches.
 */
typedef struct 
{
    size_t table_size;
    unsigned prices[POS_STATES_MAX][LEN_SYMBOLS];
    probability choice; /* low[0] is choice_2. Must be consecutive for speed */
    probability low[POS_STATES_MAX << (LEN_LOW_BITS + 1)];
    probability high[LEN_HIGH_SYMBOLS];
} LZMA2_lenStates;

/* All probabilities for the encoder. This is a separate from the encoder object
 * so the state can be saved and restored in case a chunk is not compressible.
 */
typedef struct
{
    /* Fields are ordered for speed */
    LZMA2_lenStates rep_len_states;
    probability is_rep0_long[STATES][POS_STATES_MAX];

    size_t state;
    uint32_t reps[kNumReps];

    probability is_match[STATES][POS_STATES_MAX];
    probability is_rep[STATES];
    probability is_rep_G0[STATES];
    probability is_rep_G1[STATES];
    probability is_rep_G2[STATES];

    LZMA2_lenStates len_states;

    probability dist_slot_encoders[DIST_STATES][DIST_SLOTS];
    probability dist_align_encoders[1 << ALIGN_BITS];
    probability dist_encoders[FULL_DISTANCES - DIST_MODEL_END];

    probability literal_probs[(kNumLiterals * kNumLitTables) << LZMA_LCLP_MAX];
} LZMA2_encStates;

/*
 * Linked list item for optimal parsing
 */
typedef struct
{
    size_t state;
    uint32_t price;
    unsigned extra; /*  0   : normal
                     *  1   : LIT : MATCH
                     *  > 1 : MATCH (extra-1) : LIT : REP0 (len) */
    unsigned len;
    uint32_t dist;
    uint32_t reps[kNumReps];
} LZMA2_node;

/*
 * Table and chain for 3-byte hash. Extra elements in hash_chain_3 are malloced.
 */
typedef struct {
    int32_t table_3[1 << kHash3Bits];
    int32_t hash_chain_3[1];
} LZMA2_hc3;

/*
 * LZMA2 encoder.
 */
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

    RC_encoder rc;
    /* Finish writing the chunk at this size */
    size_t chunk_size;
    /* Don't encode a symbol beyond this limit (used by fast mode) */
    size_t chunk_limit;

    LZMA2_encStates states;

    unsigned match_price_count;
    unsigned rep_len_price_count;
    size_t dist_price_table_size;
    unsigned align_prices[ALIGN_SIZE];
    unsigned dist_slot_prices[DIST_STATES][kDistTableSizeMax];
    unsigned distance_prices[DIST_STATES][FULL_DISTANCES];

    RMF_match base_match; /* Allows access to matches[-1] in LZMA_optimalParse */
    RMF_match matches[kMatchesMax];
    size_t match_count;

    LZMA2_node opt_buf[kOptimizerBufferSize];

    LZMA2_hc3* hash_buf;
    ptrdiff_t chain_mask_2;
    ptrdiff_t chain_mask_3;
    ptrdiff_t hash_dict_3;
    ptrdiff_t hash_prev_index;
    ptrdiff_t hash_alloc_3;

    /* Temp output buffer before space frees up in the match table */
    uint8_t out_buf[kTempBufferSize];
} LZMA2_ECtx;

void LZMA2_constructECtx(LZMA2_ECtx *const enc);

void LZMA2_freeECtx(LZMA2_ECtx *const enc);

int LZMA2_hashAlloc(LZMA2_ECtx *const enc, const lzma_options_lzma* const options);

size_t LZMA2_encode(LZMA2_ECtx *const enc,
    FL2_matchTable* const tbl,
	lzma_data_block const block,
	const lzma_options_lzma* const options,
	FL2_atomic *const progress_in,
    FL2_atomic *const progress_out,
    bool *const canceled);

uint8_t LZMA2_getDictSizeProp(size_t const dictionary_size);

size_t LZMA2_compressBound(size_t src_size);

size_t LZMA2_encMemoryUsage(unsigned const chain_log, lzma_mode const strategy, unsigned const thread_count);

#if defined (__cplusplus)
}
#endif

#endif /* RADYX_LZMA2_ENCODER_H */