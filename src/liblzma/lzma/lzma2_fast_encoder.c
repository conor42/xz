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
#include "radix_mf.h"
#include "lzma2_enc.h"
#include "tuklib_cpucores.h"

typedef struct {
	uint8_t* data;
	size_t start;
	size_t end;
} lzma_dict_block;

typedef struct {
	LZMA2_ECtx enc;
	lzma_data_block block;
	size_t out_size;
} lzma2_thread;

typedef struct {
	/// Flag to end reading from upstream filter
	bool ending;

	/// LZMA options currently in use.
	lzma_options_lzma opt_cur;

	/// Allocated dictionary size
	size_t dict_size;

	/// Dictionary buffer of dict_size bytes
	lzma_dict_block dict_block;

	/// Next coder in the chain
	lzma_next_coder next;

	/// Match table allocated with thread_count threads
	FL2_matchTable *match_table;

	/// Current source position for output
	size_t out_pos;

	/// Current source thread for output
	size_t out_thread;

	/// Number of encoders allocated
	size_t thread_count;

	/// Encoder thread data
	lzma2_thread encoders[1];
} lzma2_fast_coder;


#define return_if_fl2_error(expr) \
do { \
	const size_t ret_ = (expr); \
	if (FL2_isError(ret_)) \
		return ret_; \
} while (0)


static void
reset_dict(lzma2_fast_coder *coder)
{
	coder->dict_block.start = 0;
	coder->dict_block.end = 0;
}


static lzma_ret lzma2_fast_encoder_create(lzma2_fast_coder *coder,
	const lzma_allocator *allocator)
{
	RMF_parameters params;
	params.dictionary_size = coder->opt_cur.dict_size;
	params.overlap_fraction = coder->opt_cur.overlap_fraction;
	params.match_buffer_resize = RMF_DEFAULT_BUF_RESIZE;
	params.depth = coder->opt_cur.depth;
	params.divide_and_conquer = coder->opt_cur.divide_and_conquer;
	/* Free unsuitable match table before reallocating anything else */
	if (coder->match_table && !RMF_compatibleParameters(coder->match_table, &params, 0)) {
		RMF_freeMatchTable(coder->match_table);
		coder->match_table = NULL;
	}
	if (coder->dict_block.data && coder->dict_size < coder->opt_cur.dict_size) {
		lzma_free(coder->dict_block.data, allocator);
		coder->dict_block.data = NULL;
	}

	if (LZMA2_hashAlloc(&coder->encoders[0].enc, &coder->opt_cur))
		return LZMA_MEM_ERROR;

	if (!coder->match_table) {
		coder->match_table = RMF_createMatchTable(&params, 0, coder->thread_count);
		if (!coder->match_table)
			return LZMA_MEM_ERROR;
	}
	else {
		return_if_error(RMF_applyParameters(coder->match_table, &params, 0));
	}
	reset_dict(coder);
	if (!coder->dict_block.data) {
		coder->dict_size = coder->opt_cur.dict_size;
		coder->dict_block.data = lzma_alloc(coder->dict_size, allocator);
		if (!coder->dict_block.data)
			return LZMA_MEM_ERROR;
	}

	return LZMA_OK;
}

static lzma_ret
compress(lzma2_fast_coder *coder)
{
	int canceled = 0;
	coder->encoders[0].block.data = coder->dict_block.data;
	coder->encoders[0].block.start = coder->dict_block.start;
	coder->encoders[0].block.end = coder->dict_block.end;
	RMF_initTable(coder->match_table, coder->dict_block.data, coder->dict_block.end);
	RMF_buildTable(coder->match_table, 0, 0, coder->encoders[0].block);
	FL2_atomic progress_in = 0, progress_out = 0;
	size_t out_size = LZMA2_encode(&coder->encoders[0].enc, coder->match_table, coder->encoders[0].block, &coder->opt_cur,
		&progress_in, &progress_out, &canceled);
	if (out_size == (size_t)-1)
		return LZMA_PROG_ERROR;
	coder->encoders[0].out_size = out_size;
	coder->out_thread = 0;
	coder->thread_count = 1;
	coder->dict_block.start = coder->dict_block.end;
	return LZMA_OK;
}

static bool
have_output(lzma2_fast_coder *coder)
{
	return coder->out_thread < coder->thread_count;
}

static bool
copy_output(lzma2_fast_coder *coder,
		uint8_t *out, size_t *out_pos, size_t out_size)
{
	for (; coder->out_thread < coder->thread_count; ++coder->out_thread)
	{
		const uint8_t* const outBuf = RMF_getTableAsOutputBuffer(coder->match_table, coder->encoders[coder->out_thread].block.start) + coder->out_pos;
		size_t const dstCapacity = out_size - *out_pos;
		size_t toWrite = coder->encoders[coder->out_thread].out_size;

		toWrite = my_min(toWrite - coder->out_pos, dstCapacity);

		DEBUGLOG(5, "CStream : writing %u bytes", (uint32_t)toWrite);

		memcpy(out + *out_pos, outBuf, toWrite);
		coder->out_pos += toWrite;
		*out_pos += toWrite;

		// If the slice is not flushed, the output is full
		if (coder->out_pos < coder->encoders[coder->out_thread].out_size)
			return true;

		coder->out_pos = 0;
	}
	return false;
}

#define ALIGNMENT_SIZE 16U
#define ALIGNMENT_MASK (~(size_t)(ALIGNMENT_SIZE-1))

static void
dict_shift(lzma2_fast_coder *coder)
{
    if (coder->dict_block.start < coder->dict_block.end)
        return;

    size_t overlap = OVERLAP_FROM_DICT_SIZE(coder->dict_size, coder->opt_cur.overlap_fraction);

	if (overlap == 0) {
		coder->dict_block.start = 0;
		coder->dict_block.end = 0;
    }
    else if (coder->dict_block.end >= overlap + ALIGNMENT_SIZE) {
        size_t const from = (coder->dict_block.end - overlap) & ALIGNMENT_MASK;
        uint8_t *const data = coder->dict_block.data;

        overlap = coder->dict_block.end - from;

        if (overlap <= from) {
            DEBUGLOG(5, "Copy overlap data : %u bytes from %u", (unsigned)overlap, (unsigned)from);
            memcpy(data, data + from, overlap);
        }
        else if (from != 0) {
            DEBUGLOG(5, "Move overlap data : %u bytes from %u", (unsigned)overlap, (unsigned)from);
            memmove(data, data + from, overlap);
        }
        /* New data will be written after the overlap */
        coder->dict_block.start = overlap;
        coder->dict_block.end = overlap;
    }
}

/// \brief      Tries to fill the input window (coder->fcs internal buffer)
///
/// If we are the last encoder in the chain, our input data is in in[].
/// Otherwise we call the next filter in the chain to process in[] and
/// write its output to coder->fcs. Copy any output the encoder generates.
///
/// This function must not be called once it has returned LZMA_STREAM_END.
///
static lzma_ret
fill_window(lzma2_fast_coder *coder, const lzma_allocator *allocator,
		const uint8_t *in, size_t *in_pos, size_t in_size,
		uint8_t *out, size_t *out_pos, size_t out_size,
		lzma_action action)
{
	// Copy any output pending in the internal buffer
	copy_output(coder, out, out_pos, out_size);

	dict_shift(coder);

	lzma_ret ret;
	if (coder->next.code == NULL) {
		// Not using a filter, simply memcpy() as much as possible.
		lzma_bufcpy(in, in_pos, in_size, coder->dict_block.data,
				&coder->dict_block.end, coder->dict_size);

		ret = action != LZMA_RUN && *in_pos == in_size
			? LZMA_STREAM_END : LZMA_OK;

	} else {
		ret = coder->next.code(coder->next.coder, allocator,
				in, in_pos, in_size,
				coder->dict_block.data, &coder->dict_block.end,
				coder->dict_size, action);
	}

	coder->ending = (ret == LZMA_STREAM_END);

	assert(coder->dict_block.end <= coder->dict_size);
	if (!have_output(coder) && coder->dict_block.end == coder->dict_size) {
		return_if_error(compress(coder));
		copy_output(coder, out, out_pos, out_size);
	}

	return LZMA_OK;
}


static lzma_ret
flush_stream(lzma2_fast_coder *coder,
		uint8_t *out, size_t *out_pos, size_t out_size)
{
	copy_output(coder, out, out_pos, out_size);

	return_if_error(compress(coder));

	copy_output(coder, out, out_pos, out_size);

	return LZMA_OK;
}

static lzma_ret
end_stream(lzma2_fast_coder *coder,
		uint8_t *out, size_t *out_pos, size_t out_size)
{
	return_if_error(flush_stream(coder, out, out_pos, out_size));

	if (*out_pos < out_size) {
		out[*out_pos] = LZMA2_END_MARKER;
		++(*out_pos);
		return LZMA_STREAM_END;
	}

	return LZMA_OK;
}


static lzma_ret
flzma2_encode(void *coder_ptr,
		const lzma_allocator *allocator,
		const uint8_t *restrict in, size_t *restrict in_pos,
		size_t in_size, uint8_t *restrict out,
		size_t *restrict out_pos, size_t out_size,
		lzma_action action)
{
	lzma2_fast_coder *restrict coder = coder_ptr;

	lzma_ret ret = LZMA_OK;

	if (!coder->ending)
		return_if_error(fill_window(coder, allocator,
			in, in_pos, in_size, out,
			out_pos, out_size, action));

	switch (action) {
	case LZMA_RUN:
		break;

	case LZMA_SYNC_FLUSH:
		// Return LZMA_OK if output or input not done
		if (!coder->ending)
			break;

		return_if_error(flush_stream(coder, out, out_pos, out_size));

		if (!have_output(coder))
			ret = LZMA_STREAM_END;

		break;

	case LZMA_FULL_FLUSH:
	case LZMA_FULL_BARRIER:
		// Return LZMA_OK if input not done.
		if (!coder->ending)
			break;

		return_if_error(flush_stream(coder, out, out_pos, out_size));

		if (!have_output(coder)) {
			ret = LZMA_STREAM_END;
			// Re-initialize for next block
			reset_dict(coder);
			coder->ending = false;
		}

		break;

	case LZMA_FINISH:
		if (coder->ending) {
			ret = end_stream(coder, out, out_pos, out_size);
			return_if_error(ret);
		}
		break;
	}

	return ret;
}


static void
flzma2_encoder_end(void *coder_ptr, const lzma_allocator *allocator)
{
	lzma2_fast_coder *coder = coder_ptr;

	lzma_next_end(&coder->next, allocator);

	lzma_free(coder->dict_block.data, allocator);
	RMF_freeMatchTable(coder->match_table);

	for(size_t i = 0; i < coder->thread_count; ++i)
		LZMA2_freeECtx(&coder->encoders[i].enc);

	lzma_free(coder, allocator);
}


static void
get_progress(void *coder_ptr, uint64_t *progress_in, uint64_t *progress_out)
{
	lzma2_fast_coder *coder = coder_ptr;

	*progress_out = 0;// fcs->streamCsize + fcs->progressOut;
#if 0
	uint64_t const encodeSize = coder->dict_block.end - coder->dict_block.start;

	if (fcs->progressIn == 0 && fcs->curBlock.end != 0)
		return fcs->streamTotal + ((fcs->matchTable->progress * encodeSize / fcs->curBlock.end * fcs->rmfWeight) >> 4);

	return fcs->streamTotal + ((fcs->rmfWeight * encodeSize) >> 4) + ((fcs->progressIn * fcs->encWeight) >> 4);
#endif
	*progress_in = 0;// FL2_getCStreamProgress(coder->fcs, &out);
//	*progress_out = out;
}


static lzma_ret
flzma2_encoder_options_update(void *coder_ptr, const lzma_allocator *allocator,
	const lzma_filter *filters lzma_attribute((__unused__)),
	const lzma_filter *reversed_filters)
{
	lzma2_fast_coder *coder = coder_ptr;

	if (reversed_filters->options == NULL)
		return LZMA_PROG_ERROR;

	// Look if there are new options. At least for now,
	// only lc/lp/pb can be changed.
	const lzma_options_lzma *opt = reversed_filters->options;
	if (coder->opt_cur.lc != opt->lc || coder->opt_cur.lp != opt->lp
			|| coder->opt_cur.pb != opt->pb) {
		// Validate the options.
		if (opt->lc > LZMA_LCLP_MAX || opt->lp > LZMA_LCLP_MAX
				|| opt->lc + opt->lp > LZMA_LCLP_MAX
				|| opt->pb > LZMA_PB_MAX)
			return LZMA_OPTIONS_ERROR;

		// The new options will be used when the encoder starts
		// a new dictionary block.
		coder->opt_cur.lc = opt->lc;
		coder->opt_cur.lp = opt->lp;
		coder->opt_cur.pb = opt->pb;
	}

	return lzma_next_filter_update(
		&coder->next, allocator, reversed_filters + 1);
}


extern lzma_ret
lzma_flzma2_encoder_init(lzma_next_coder *next,
		const lzma_allocator *allocator,
		const lzma_filter_info *filters)
{
	if (filters[0].options == NULL)
		return LZMA_PROG_ERROR;

	const lzma_options_lzma *options = filters[0].options;
	unsigned threads = (options->threads == 0) ? tuklib_cpucores() : options->threads;
	threads += !threads;

	lzma2_fast_coder *coder = next->coder;
	if (coder == NULL) {
		coder = lzma_alloc_zero(sizeof(lzma2_fast_coder) + (threads - 1) * sizeof(lzma2_thread), allocator);
		if (coder == NULL)
			return LZMA_MEM_ERROR;

		coder->dict_block.data = NULL;
		coder->match_table = NULL;
		LZMA2_constructECtx(&coder->encoders[0].enc);

		next->coder = coder;
		next->code = &flzma2_encode;
		next->end = &flzma2_encoder_end;
		next->get_progress = &get_progress;
		next->update = &flzma2_encoder_options_update;

		coder->next = LZMA_NEXT_CODER_INIT;
	}

	coder->opt_cur = *options;
	if (options->depth == 0)
		coder->opt_cur.depth = 42 + (options->dict_size >> 25) * 4U;

	return_if_error(lzma2_fast_encoder_create(coder, allocator));

	coder->ending = false;

	// Initialize the next filter in the chain, if any.
	return lzma_next_filter_init(&coder->next, allocator, filters + 1);
}


extern uint64_t
lzma_flzma2_encoder_memusage(const void *options)
{
	const lzma_options_lzma *const opt = options;

	return 0;// FL2_estimateCStreamSize_byParams(&params, opt->threads, 0);// opt->dual_buffer);
}
