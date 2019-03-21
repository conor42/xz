///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma2_fast_encoder.c
/// \brief      Fast LZMA2 encoder wrapper
///
//  Authors:    Conor McCarthy
//              Lasse Collin
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "lzma2_fast_encoder.h"
#include "fast-lzma2.h"
#include "fl2_errors.h"


typedef struct {
	/// Fast LZMA2 encoder
	FL2_CStream *fcs;

	/// Next coder in the chain
	lzma_next_coder next;
} flzma2_coder;


extern lzma_ret
flzma2_translate_error(const size_t ret)
{
	if (FL2_isTimedOut(ret))
		return LZMA_TIMED_OUT;

	switch (FL2_getErrorCode(ret)) {

	case FL2_error_no_error:
		return LZMA_OK;

	case FL2_error_corruption_detected:
	case FL2_error_checksum_wrong:
		return LZMA_DATA_ERROR;

	case FL2_error_parameter_unsupported:
	case FL2_error_parameter_outOfBound:
	case FL2_error_lclpMax_exceeded:
		return LZMA_OPTIONS_ERROR;

	case FL2_error_memory_allocation:
		return LZMA_MEM_ERROR;

	case FL2_error_buffer:
		return LZMA_BUF_ERROR;

	case FL2_error_GENERIC:
	case FL2_error_canceled:
	case FL2_error_stage_wrong:
	case FL2_error_init_missing:
	case FL2_error_internal:
	default:
		return LZMA_PROG_ERROR;
	}
}


#define return_if_fl2_error(expr) \
do { \
	const size_t ret_ = (expr); \
	if (FL2_isError(ret_)) \
		return ret_; \
} while (0)

#define ret_translate_if_error(expr) \
do { \
	const size_t ret_ = (expr); \
	if (FL2_isError(ret_)) \
		return flzma2_translate_error(ret_); \
} while (0)


/// \brief      Tries to fill the input window (coder->fcs internal buffer)
///
/// If we are the last encoder in the chain, our input data is in in[].
/// Otherwise we call the next filter in the chain to process in[] and
/// write its output to coder->fcs. Copy any output the encoder generates.
///
static size_t
fill_window(flzma2_coder *coder, const lzma_allocator *allocator,
		const uint8_t *in, size_t *in_pos, size_t in_size,
		FL2_outBuffer *output,
		lzma_action action)
{
	FL2_dictBuffer dict;
	return_if_fl2_error(FL2_getDictionaryBuffer(coder->fcs, &dict));

	size_t write_pos = 0;

	if (coder->next.code == NULL) {
		// Not using a filter, simply memcpy() as much as possible.
		lzma_bufcpy(in, in_pos, in_size, dict.dst,
				&write_pos, dict.size);
	} else {
		coder->next.code(coder->next.coder, allocator,
				in, in_pos, in_size,
				dict.dst, &write_pos,
				dict.size, action);
	}

	// Blocks for compression if dict is full
	size_t res = FL2_updateDictionary(coder->fcs, write_pos);
	return_if_fl2_error(res);

	if(res != 0)
		res = FL2_copyCStreamOutput(coder->fcs, output);

	return res;
}


static lzma_ret
flzma2_encode(void *coder_ptr,
	const lzma_allocator *allocator lzma_attribute((__unused__)),
	const uint8_t *restrict in, size_t *restrict in_pos,
	size_t in_size, uint8_t *restrict out,
	size_t *restrict out_pos, size_t out_size,
	lzma_action action)
{
	flzma2_coder *restrict coder = coder_ptr;

	FL2_outBuffer output = { out, out_size, *out_pos };

	lzma_ret ret = LZMA_OK;

	size_t res = 0;

	do {
		res = fill_window(coder, allocator,
				in, in_pos, in_size, &output, action);
		return_if_fl2_error(res);
	} while (res == 0 && *in_pos < in_size);

	switch (action) {
	case LZMA_RUN:
		break;

	case LZMA_SYNC_FLUSH:
	case LZMA_FULL_FLUSH:
	case LZMA_FULL_BARRIER:
		// Return LZMA_OK if output is full
		if (res != 0)
			break;

		// The loop above will consume all input if the output never fills
		assert(*in_pos == in_size);

		res = FL2_flushStream(coder->fcs, &output);
		ret_translate_if_error(res);

		// Copy output if present and return LZMA_OK if output is full
		if (res != 0 && FL2_copyCStreamOutput(coder->fcs, &output) != 0)
			break;

		ret = LZMA_STREAM_END;

		if (action != LZMA_SYNC_FLUSH) {
			// Not done unless there's room for the terminator byte
			ret = LZMA_OK;
			// End the stream if full flushing
			res = FL2_endStream(coder->fcs, &output);

			ret_translate_if_error(res);

			if (res == 0) {
				ret = LZMA_STREAM_END;
				// Re-initialize for next block
				FL2_initCStream(coder->fcs, 0);
			}
		}

		break;

	case LZMA_FINISH:
		if (res == 0 && *in_pos == in_size) {
			res = FL2_endStream(coder->fcs, &output);

			ret_translate_if_error(res);

			if (res == 0)
				ret = LZMA_STREAM_END;
		}
		break;
	}

	if (FL2_isError(res))
		return flzma2_translate_error(res);

	*out_pos = output.pos;
	return ret;
}

static void
flzma2_encoder_end(void *coder_ptr, const lzma_allocator *allocator)
{
	flzma2_coder *coder = coder_ptr;

	lzma_next_end(&coder->next, allocator);

	FL2_freeCStream(coder->fcs);

	lzma_free(coder, allocator);
}


static lzma_ret
flzma2_set_options(flzma2_coder *coder, const lzma_options_lzma *options)
{
	if (coder == NULL)
		return LZMA_PROG_ERROR;

	FL2_CStream *fcs = coder->fcs;
	if (fcs == NULL)
		return LZMA_PROG_ERROR;

	uint32_t depth = options->depth;
	if (depth == 0)
		depth = 42 + (options->dict_size >> 25) * 4U;

	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_dictionarySize, options->dict_size));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_overlapFraction, options->overlap_fraction));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_hybridChainLog, options->near_dict_size_log));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_searchDepth, depth));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_hybridCycles, options->near_depth));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_divideAndConquer, options->divide_and_conquer));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_strategy, options->mode - 1));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_literalCtxBits, options->lc));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_literalPosBits, options->lp));
	ret_translate_if_error(FL2_CStream_setParameter(fcs, FL2_p_posBits, options->pb));

	return LZMA_OK;
}


static lzma_ret
flzma2_encoder_options_update(void *coder_ptr, const lzma_allocator *allocator,
	const lzma_filter *filters lzma_attribute((__unused__)),
	const lzma_filter *reversed_filters)
{
	flzma2_coder *coder = coder_ptr;

	// New options can be set only when there is no incomplete chunk.
	// This is the case at the beginning of the raw stream and right
	// after LZMA_SYNC_FLUSH.
	if (reversed_filters->options == NULL)
		return LZMA_PROG_ERROR;

	// Look if there are new options. At least for now,
	// only lc/lp/pb can be changed.
	const lzma_options_lzma *opt = reversed_filters->options;

	if (opt->lc + opt->lp > LZMA_LCLP_MAX)
		return LZMA_OPTIONS_ERROR;

	ret_translate_if_error(FL2_CStream_setParameter(coder->fcs, FL2_p_literalCtxBits, opt->lc));
	ret_translate_if_error(FL2_CStream_setParameter(coder->fcs, FL2_p_literalPosBits, opt->lp));
	ret_translate_if_error(FL2_CStream_setParameter(coder->fcs, FL2_p_posBits, opt->pb));

	return lzma_next_filter_update(
		&coder->next, allocator, reversed_filters + 1);

	return LZMA_OK;
}


extern lzma_ret
lzma_flzma2_encoder_init(lzma_next_coder *next, const lzma_allocator *allocator,
		const lzma_filter_info *filters)
{
	if (filters[0].options == NULL)
		return LZMA_PROG_ERROR;

	const lzma_options_lzma *options = filters[0].options;

	flzma2_coder *coder = next->coder;
	if (coder == NULL) {
		coder = lzma_alloc(sizeof(flzma2_coder), allocator);
		if (coder == NULL)
			return LZMA_MEM_ERROR;

		coder->fcs = NULL;

		next->coder = coder;
		next->code = &flzma2_encode;
		next->end = &flzma2_encoder_end;
		next->update = &flzma2_encoder_options_update;

		coder->next = LZMA_NEXT_CODER_INIT;
	}

	if (coder->fcs == NULL) {
		coder->fcs = FL2_createCStreamMt(options->threads, 0);// options->dual_buffer);
		if (coder->fcs == NULL)
			return LZMA_MEM_ERROR;
	}

	return_if_error(flzma2_set_options(coder, options));

	ret_translate_if_error(FL2_CStream_setParameter(coder->fcs, FL2_p_omitProperties, 1));
	ret_translate_if_error(FL2_CStream_setParameter(coder->fcs, FL2_p_resetInterval, 0));

	ret_translate_if_error(FL2_initCStream(coder->fcs, 0));

	// Initialize the next filter in the chain, if any.
	return lzma_next_filter_init(&coder->next, allocator, filters + 1);
}


extern uint64_t
lzma_flzma2_encoder_memusage(const void *options)
{
	const lzma_options_lzma *const opt = options;
	FL2_compressionParameters params;

	params.dictionarySize = opt->dict_size;
	params.bufferLog = 4;
	params.chainLog = 9;// opt->hc3_dict_size_log;
	params.strategy = opt->mode;

	return FL2_estimateCStreamSize_byParams(&params, 0, 0);// opt->threads, opt->dual_buffer);
}
