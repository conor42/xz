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


extern lzma_ret flzma2_translate_error(const size_t ret)
{
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
		return flzma2_translate_error(ret_); \
} while (0)


static lzma_ret
flzma2_encode(void *coder_ptr,
	const lzma_allocator *allocator lzma_attribute((__unused__)),
	const uint8_t *restrict in, size_t *restrict in_pos,
	size_t in_size, uint8_t *restrict out,
	size_t *restrict out_pos, size_t out_size,
	lzma_action action)
{
	flzma2_coder *restrict coder = coder_ptr;

	FL2_outBuffer outbuf = { out, out_size, *out_pos };
	FL2_inBuffer inbuf = { in, in_size, *in_pos };

	size_t res = 0;

	lzma_ret ret = LZMA_OK;

	switch (action) {
	case LZMA_RUN:
		res = FL2_compressStream(coder->fcs, &outbuf, &inbuf);
		break;

	case LZMA_SYNC_FLUSH:
		res = FL2_compressStream(coder->fcs, &outbuf, &inbuf);
		if (FL2_isError(res))
			return flzma2_translate_error(res);

		// fall through

	case LZMA_FULL_FLUSH:
	case LZMA_FULL_BARRIER:
		res = FL2_flushStream(coder->fcs, &outbuf);
		if (action == LZMA_FULL_BARRIER && !FL2_isError(res))
			res = FL2_flushStream(coder->fcs, &outbuf);

		if (FL2_isError(res))
			return flzma2_translate_error(res);

		ret = (res != 0) ? LZMA_OK : LZMA_STREAM_END;
		break;

	case LZMA_FINISH:
		if (inbuf.pos < inbuf.size) {
			return_if_fl2_error(FL2_compressStream(coder->fcs, &outbuf, &inbuf));
		}
		else {
			res = FL2_endStream(coder->fcs, &outbuf);
			return_if_fl2_error(res);
			if (res == 0) ret = LZMA_STREAM_END;
		}
		break;
	}

	if (FL2_isError(res))
		return flzma2_translate_error(res);

	*in_pos = inbuf.pos;
	*out_pos = outbuf.pos;
	return ret;
}

static void
flzma2_encoder_end(void *coder_ptr, const lzma_allocator *allocator)
{
	flzma2_coder *coder = coder_ptr;
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

	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_dictionarySize, options->dict_size));
//	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_overlapFraction, options->overlap_fraction));
//	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_chainLog, options->hc3_dict_size_log));
	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_searchDepth, options->depth));
//	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_hybridCycles, options->hc3_cycles));
//	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_divideAndConquer, options->divide_and_conquer));
	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_strategy, options->mode));
	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_literalCtxBits, options->lc));
	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_literalPosBits, options->lp));
	return_if_fl2_error(FL2_CStream_setParameter(fcs, FL2_p_posBits, options->pb));

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

	return_if_fl2_error(FL2_CStream_setParameter(coder->fcs, FL2_p_literalCtxBits, opt->lc));
	return_if_fl2_error(FL2_CStream_setParameter(coder->fcs, FL2_p_literalPosBits, opt->lp));
	return_if_fl2_error(FL2_CStream_setParameter(coder->fcs, FL2_p_posBits, opt->pb));

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
		coder->fcs = FL2_createCStreamMt(0, 0);// options->threads, options->dual_buffer);
		if (coder->fcs == NULL)
			return LZMA_MEM_ERROR;
	}

	return_if_error(flzma2_set_options(coder, options));

	return_if_fl2_error(FL2_CStream_setParameter(coder->fcs, FL2_p_omitProperties, 1));
	return_if_fl2_error(FL2_CStream_setParameter(coder->fcs, FL2_p_resetInterval, 0));

	return_if_fl2_error(FL2_initCStream(coder->fcs, 0));

	return LZMA_OK;
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
