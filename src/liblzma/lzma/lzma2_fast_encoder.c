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
#include "lzma2_encoder_rmf.h"
#include "tuklib_cpucores.h"
#include "mythread.h"

#define LZMA2_TIMEOUT 300


typedef enum {
	/// Waiting for work.
	THR_IDLE,

	/// Match table is under construction.
	THR_BUILD,

	/// Encoding is in progress.
	THR_ENC,

	/// The main thread wants the thread to exit.
	THR_EXIT,

} worker_state;


typedef struct lzma2_fast_coder_s lzma2_fast_coder;

typedef struct {
	lzma2_fast_coder *coder;
	rmf_builder *builder;
	lzma2_rmf_encoder enc;
	lzma_data_block block;
	size_t out_size;
#ifdef MYTHREAD_ENABLED
	mythread thread_id;
	mythread_mutex mutex;
	mythread_cond cond;
	worker_state state;
#endif
} worker_thread;


typedef struct {
	uint8_t* data;
	size_t start;
	size_t end;
} lzma_dict_block;


struct lzma2_fast_coder_s {
	/// Flag to end reading from upstream filter.
	bool ending;

	/// LZMA options currently in use.
	lzma_options_lzma opt_cur;

	/// Allocated dictionary size.
	size_t dict_size;

	/// Dictionary buffer of dict_size bytes.
	lzma_dict_block dict_block;

	/// Next coder in the chain.
	lzma_next_coder next;

	/// Match table allocated with thread_count threads.
	rmf_match_table *match_table;

	/// Current source position for output.
	size_t out_pos;

	/// Current source thread for output.
	size_t out_thread;

	/// Number of thread structs allocated.
	size_t thread_count;

	/// Progress weight of the match-finder stage.
	unsigned rmf_weight;

	/// Progress weight of the encoder stage.
	unsigned enc_weight;

	/// Amount of uncompressed data compressed by running encoders.
	lzma_atomic progress_in;

	/// Amount of compressed data buffered by running encoders.
	lzma_atomic progress_out;

	/// Amount of uncompressed data that has already been compressed.
	uint64_t total_in;

	/// Amount of compressed data that is ready.
	uint64_t total_out;

	/// Worker thread sequence.
	enum {
		CODER_BUILD,
		CODER_ENC,
		CODER_WRITE
	} sequence;

	/// Flag to stop async encoder
	bool canceled;

	/// Encoder thread data
	worker_thread *threads;
};


static void
reset_dict(lzma2_fast_coder *coder)
{
	coder->dict_block.start = 0;
	coder->dict_block.end = 0;
}


static lzma_ret
create_builders(lzma2_fast_coder *coder, const lzma_allocator *allocator)
{
	for (size_t i = 0; i < coder->thread_count; ++i) {
		coder->threads[i].builder = rmf_create_builder(coder->match_table, coder->threads[i].builder, allocator);
		if (!coder->threads[i].builder)
			return LZMA_MEM_ERROR;
	}
	return LZMA_OK;
}


static void
free_builders(lzma2_fast_coder *coder, const lzma_allocator *allocator)
{
	for (size_t i = 0; i < coder->thread_count; ++i) {
		lzma_free(coder->threads[i].builder, allocator);
		coder->threads[i].builder = NULL;
	}
}


static void set_weights(lzma2_fast_coder *coder)
{
	uint32_t rmf_weight = bsr32((uint32_t)coder->dict_block.end);
	uint32_t depth_weight = 2 + (coder->opt_cur.depth >= 12) + (coder->opt_cur.depth >= 28);
	uint32_t enc_weight;

	if (rmf_weight >= 20) {
		rmf_weight = depth_weight * (rmf_weight - 10) + (rmf_weight - 19) * 12;
		if (coder->opt_cur.mode == LZMA_MODE_FAST)
			enc_weight = 20;
		else if (coder->opt_cur.mode == LZMA_MODE_NORMAL)
			enc_weight = 50;
		else
			enc_weight = 60 + coder->opt_cur.near_dict_size_log + bsr32(coder->opt_cur.nice_len) * 3U;
		rmf_weight = (rmf_weight << 4) / (rmf_weight + enc_weight);
		enc_weight = 16 - rmf_weight;
	}
	else {
		rmf_weight = 8;
		enc_weight = 8;
	}

	coder->rmf_weight = rmf_weight;
	coder->enc_weight = enc_weight;
}


#ifdef MYTHREAD_ENABLED


static MYTHREAD_RET_TYPE
worker_start(void *thr_ptr)
{
	worker_thread *thr = thr_ptr;
	worker_state state = THR_IDLE; // Init to silence a warning

	while (true) {
		// Wait for work.
		mythread_sync(thr->mutex)
		{
			while (true) {
				state = thr->state;
				if (state != THR_IDLE)
					break;

				mythread_cond_wait(&thr->cond, &thr->mutex);
			}
		}

		assert(state != THR_IDLE);

		if (state == THR_EXIT)
			break;

		lzma2_fast_coder *coder = thr->coder;
		if (state == THR_BUILD) {
			lzma_data_block block = { coder->dict_block.data,
				coder->dict_block.start,
				coder->dict_block.end };
			rmf_build_table(coder->match_table, thr->builder, thr != coder->threads, block);
		}
		else {
			assert(state == THR_ENC);
			thr->out_size = lzma2_rmf_encode(&thr->enc, coder->match_table, thr->block, &coder->opt_cur,
				&coder->progress_in, &coder->progress_out, &coder->canceled);
		}

		// Mark the thread as idle unless the main thread has
		// told us to exit. Signal is needed for the case
		// where the main thread is waiting for the threads to stop.
		mythread_sync(thr->mutex)
		{
			if (thr->state != THR_EXIT) {
				thr->state = THR_IDLE;
				mythread_cond_signal(&thr->cond);
			}
		}
	}

	// Exiting, free the resources.
	mythread_mutex_destroy(&thr->mutex);
	mythread_cond_destroy(&thr->cond);

	return MYTHREAD_RET_VALUE;
}


static lzma_ret
thread_initialize(lzma2_fast_coder *coder, size_t i)
{
	coder->threads[i].coder = coder;
	coder->threads[i].builder = NULL;
	coder->threads[i].state = THR_IDLE;
	lzma2_rmf_enc_construct(&coder->threads[i].enc);

	if(mythread_mutex_init(&coder->threads[i].mutex))
		return LZMA_MEM_ERROR;
	if(mythread_cond_init(&coder->threads[i].cond))
		goto error_cond;
	if (mythread_create(&coder->threads[i].thread_id,
			&worker_start, coder->threads + i) == 0)
		return LZMA_OK;

	mythread_cond_destroy(&coder->threads[i].cond);
error_cond:
	mythread_mutex_destroy(&coder->threads[i].mutex);
	return LZMA_MEM_ERROR;
}


static void
thread_free(lzma2_fast_coder *coder, size_t i)
{
	mythread_sync(coder->threads[i].mutex) {
		coder->threads[i].state = THR_EXIT;
		mythread_cond_signal(&coder->threads[i].cond);
	}
	int ret = mythread_join(coder->threads[i].thread_id);
	assert(ret == MYTHREAD_RET_VALUE);
	(void)ret;
}


static inline size_t
rmf_thread_count(lzma2_fast_coder *coder)
{
	size_t rmf_threads = coder->dict_block.end / RMF_MIN_BYTES_PER_THREAD;
	rmf_threads = my_min(coder->thread_count, rmf_threads);
	rmf_threads = my_min(rmf_threads, coder->opt_cur.threads);
	return rmf_threads + !rmf_threads;
}


// Each encoder thread begins with default probabilities. Ensure the slices are not so small
// that the ratio is poor.
static inline size_t
enc_thread_count(lzma2_fast_coder *coder)
{
	size_t const encode_size = (coder->dict_block.end - coder->dict_block.start);
	size_t enc_threads = my_min(coder->thread_count, encode_size / ENC_MIN_BYTES_PER_THREAD);
	enc_threads = my_min(enc_threads, coder->opt_cur.threads);
	return enc_threads + !enc_threads;
}


static inline void
builder_run(lzma2_fast_coder *coder, size_t i)
{
	mythread_sync(coder->threads[i].mutex)
	{
		coder->threads[i].state = THR_BUILD;
		mythread_cond_signal(&coder->threads[i].cond);
	}
}


static inline void
encoder_run(lzma2_fast_coder *coder, size_t i)
{
	mythread_sync(coder->threads[i].mutex)
	{
		coder->threads[i].state = THR_ENC;
		mythread_cond_signal(&coder->threads[i].cond);
	}
}


static lzma_ret
threads_wait(lzma2_fast_coder *coder)
{
	// Wait for the threads to settle in the idle state.
	for (uint32_t i = 0; i < coder->thread_count; ++i) {
		if (coder->threads[i].state == THR_IDLE)
			continue;
		bool timed_out = false;
		mythread_condtime wait_abs;
		mythread_condtime_set(&wait_abs, &coder->threads[i].cond, LZMA2_TIMEOUT);
		mythread_sync(coder->threads[i].mutex) {
			while (coder->threads[i].state != THR_IDLE && !timed_out)
				timed_out = mythread_cond_timedwait(&coder->threads[i].cond,
						&coder->threads[i].mutex, &wait_abs) != 0;
		}
		if(timed_out)
			return LZMA_TIMED_OUT;
	}
	return LZMA_OK;
}


static bool
working(lzma2_fast_coder *coder)
{
	for (size_t i = 0; i < coder->thread_count; ++i)
		if (coder->threads[i].state != THR_IDLE)
			return true;
	return false;
}


#else // MYTHREAD_ENABLED


static inline lzma_ret
thread_initialize(lzma2_fast_coder *coder lzma_attribute((__unused__)),
		size_t i lzma_attribute((__unused__)))
{
	assert(i == 0);
	coder->threads[i].coder = coder;
	coder->threads[i].builder = NULL;
	return LZMA_OK;
}


static void
thread_free(lzma2_fast_coder *coder lzma_attribute((__unused__)),
		size_t i lzma_attribute((__unused__)))
{
}


static inline size_t
rmf_thread_count(lzma2_fast_coder *coder lzma_attribute((__unused__)))
{
	return 1;
}


static inline size_t
enc_thread_count(lzma2_fast_coder *coder lzma_attribute((__unused__)))
{
	return 1;
}


static inline void
builder_run(lzma2_fast_coder *coder, size_t i)
{
	assert(i == 0);
	worker_thread *thr = coder->threads + i;
	lzma_data_block block = { coder->dict_block.data, coder->dict_block.start,coder->dict_block.end };
	rmf_build_table(coder->match_table, thr->builder, -1, block);
}


static inline void
encoder_run(lzma2_fast_coder *coder, size_t i)
{
	assert(i == 0);
	worker_thread *thr = coder->threads + i;
	thr->out_size = lzma2_rmf_encode(&thr->enc, coder->match_table, thr->block, &coder->opt_cur,
		&coder->progress_in, &coder->progress_out, &coder->canceled);
}


static inline lzma_ret
threads_wait(lzma2_fast_coder *coder lzma_attribute((__unused__)))
{
	return LZMA_OK;
}


static bool
working(lzma2_fast_coder *coder lzma_attribute((__unused__)))
{
	return false;
}


#endif // MYTHREAD_ENABLED


static lzma_ret
threads_run_sequence(lzma2_fast_coder *coder)
{
	return_if_error(threads_wait(coder));

	assert(coder->dict_block.start < coder->dict_block.end);

	if (coder->sequence == CODER_BUILD) {
		size_t rmf_threads = rmf_thread_count(coder);
		for (size_t i = 0; i < rmf_threads; ++i)
			builder_run(coder, i);
		coder->sequence = CODER_ENC;
		return_if_error(threads_wait(coder));
	}
	if (coder->sequence == CODER_ENC) {
		for (size_t i = 0; i < coder->thread_count && coder->threads[i].block.end != 0; ++i)
			encoder_run(coder, i);
		coder->sequence = CODER_WRITE;
		return_if_error(threads_wait(coder));
	}

	assert(coder->sequence == CODER_WRITE);

	for (size_t i = 0; i < coder->thread_count; ++i)
		if (coder->threads[i].out_size == (size_t)-1)
			return LZMA_PROG_ERROR;

	coder->total_in += coder->progress_in;
	coder->total_out += coder->progress_out;
	coder->progress_in = 0;
	coder->progress_out = 0;

	coder->out_thread = 0;
	coder->dict_block.start = coder->dict_block.end;

	return LZMA_OK;
}


static lzma_ret
compress(lzma2_fast_coder *coder)
{
	size_t const encode_size = (coder->dict_block.end - coder->dict_block.start);
	if(!encode_size)
		return LZMA_OK;

	// Fill the overrun area to silence valgrind.
	// Any matches that extend beyond dict_block.end are trimmed by the encoder.
	memset(coder->dict_block.data + coder->dict_block.end, 0xDB, coder->opt_cur.depth);
	assert(coder->opt_cur.depth <= MAX_READ_BEYOND_DEPTH);

	assert(coder->out_thread >= coder->thread_count);

	set_weights(coder);

	size_t enc_threads = enc_thread_count(coder);
	size_t slice_start = coder->dict_block.start;
	size_t const slice_size = encode_size / enc_threads;
	size_t i;

	assert(slice_size);
	for (i = 0; i < enc_threads; ++i) {
		coder->threads[i].block.data = coder->dict_block.data;
		coder->threads[i].block.start = slice_start;
		coder->threads[i].block.end = (i == enc_threads - 1)
			? coder->dict_block.end
			: slice_start + slice_size;
		slice_start += slice_size;
	}
	// Set the remaining threads to zero input and output.
	for (; i < coder->thread_count; ++i) {
		coder->threads[i].block.end = 0;
		coder->threads[i].out_size = 0;
	}

	// Initialize the table to depth 2. This operation is single-threaded.
	rmf_init_table(coder->match_table, coder->dict_block.data, coder->dict_block.end);

	coder->sequence = CODER_BUILD;
	return_if_error(threads_run_sequence(coder));

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
		const uint8_t* const out_buf = rmf_output_buffer(coder->match_table, coder->threads[coder->out_thread].block.start) + coder->out_pos;
		size_t const dst_capacity = out_size - *out_pos;
		size_t to_write = coder->threads[coder->out_thread].out_size;

		to_write = my_min(to_write - coder->out_pos, dst_capacity);

		memcpy(out + *out_pos, out_buf, to_write);
		coder->out_pos += to_write;
		*out_pos += to_write;

		// If the slice is not flushed, the output is full
		if (coder->out_pos < coder->threads[coder->out_thread].out_size)
			return true;

		coder->out_pos = 0;
	}
	return false;
}


#define ALIGNMENT_SIZE (1U << LZMA_LCLP_MAX)
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

		if (overlap <= from)
			memcpy(data, data + from, overlap);
		else if (from != 0)
			memmove(data, data + from, overlap);
		// New data will be written after the overlap.
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
	// If the dictionary is full, move/copy the overlap section to the start.
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
	if (!have_output(coder)) {
		return_if_error(compress(coder));
		copy_output(coder, out, out_pos, out_size);
	}

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

	// Continue compression if called after a timeout.
	if (working(coder))
		return_if_error(threads_run_sequence(coder));

	// Copy any output pending in the internal buffer
	copy_output(coder, out, out_pos, out_size);

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
get_progress(void *coder_ptr, uint64_t *progress_in, uint64_t *progress_out)
{
	lzma2_fast_coder *coder = coder_ptr;

	uint64_t const encode_size = coder->dict_block.end - coder->dict_block.start;

	if (coder->progress_in == 0 && coder->dict_block.end != 0)
		*progress_in = coder->total_in + ((coder->match_table->progress * encode_size / coder->dict_block.end * coder->rmf_weight) >> 4);
	else if (encode_size)
		*progress_in = coder->total_in + ((coder->rmf_weight * encode_size) >> 4) + ((coder->progress_in * coder->enc_weight) >> 4);
	else
		*progress_in = coder->total_in + coder->progress_in;

	*progress_out = coder->total_out + coder->progress_out;
}


/// Make the threads stop but not exit. Optionally wait for them to stop.
static void
threads_stop(lzma2_fast_coder *coder)
{
	if (working(coder)) {
		rmf_cancel_build(coder->match_table);
		coder->canceled = true;
		threads_wait(coder);
		rmf_reset_incomplete_build(coder->match_table);
		coder->canceled = false;
	}
}


static void
free_threads(lzma2_fast_coder *coder, const lzma_allocator *allocator)
{
	threads_stop(coder);
	for (size_t i = 0; i < coder->thread_count; ++i)
		thread_free(coder, i);
	coder->thread_count = 0;
	lzma_free(coder->threads, allocator);
	coder->threads = NULL;
}


static void
flzma2_encoder_end(void *coder_ptr, const lzma_allocator *allocator)
{
	lzma2_fast_coder *coder = coder_ptr;

	lzma_next_end(&coder->next, allocator);

	free_builders(coder, allocator);
	for (size_t i = 0; i < coder->thread_count; ++i)
		lzma2_rmf_enc_free(&coder->threads[i].enc);
	free_threads(coder, allocator);

	lzma_free(coder->dict_block.data, allocator);
	rmf_free_match_table(coder->match_table, allocator);

	lzma_free(coder, allocator);
}


static lzma_ret
create_threads(lzma2_fast_coder *coder, const lzma_allocator *allocator)
{
	uint32_t thread_count = coder->opt_cur.threads;

	if (coder->threads && coder->thread_count < thread_count)
		free_threads(coder, allocator);
	else
		threads_stop(coder);

	if (!coder->threads) {
		coder->threads = lzma_alloc(thread_count * sizeof(worker_thread), allocator);
		if (!coder->threads)
			return LZMA_MEM_ERROR;

		for (coder->thread_count = 0; coder->thread_count < thread_count; ++coder->thread_count)
			return_if_error(thread_initialize(coder, coder->thread_count));
	}
	coder->out_thread = coder->thread_count;
	return LZMA_OK;
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
		if (!is_lclppb_valid(opt))
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


static lzma_ret
lzma2_fast_encoder_create(lzma2_fast_coder *coder,
		const lzma_allocator *allocator)
{
	return_if_error(create_threads(coder, allocator));

	// Free unsuitable structures and buffers before reallocating anything.
	if (coder->match_table && !rmf_compatible_parameters(coder->match_table, coder->threads[0].builder, &coder->opt_cur)) {
		rmf_free_match_table(coder->match_table, allocator);
		coder->match_table = NULL;
		free_builders(coder, allocator);
	}
	if (coder->dict_block.data && coder->dict_size < coder->opt_cur.dict_size) {
		lzma_free(coder->dict_block.data, allocator);
		coder->dict_block.data = NULL;
	}

	for (size_t i = 0; i < coder->thread_count; ++i)
		if (lzma2_rmf_hash_alloc(&coder->threads[i].enc, &coder->opt_cur))
			return LZMA_MEM_ERROR;

	if (!coder->match_table) {
		coder->match_table = rmf_create_match_table(&coder->opt_cur, allocator);
		if (!coder->match_table)
			return LZMA_MEM_ERROR;
	}
	else {
		rmf_apply_parameters(coder->match_table, &coder->opt_cur);
	}

	return_if_error(create_builders(coder, allocator));

	reset_dict(coder);
	if (!coder->dict_block.data) {
		coder->dict_size = coder->opt_cur.dict_size;
		coder->dict_block.data = lzma_alloc(coder->dict_size + MAX_READ_BEYOND_DEPTH, allocator);
		if (!coder->dict_block.data)
			return LZMA_MEM_ERROR;
	}

	return LZMA_OK;
}


static bool
is_options_valid(const lzma_options_lzma *options)
{
	// Validate some of the options. LZ encoder validates nice_len too
	// but we need a valid value here earlier.
	return is_lclppb_valid(options)
		&& options->nice_len >= MATCH_LEN_MIN
		&& options->nice_len <= MATCH_LEN_MAX
		&& (options->mode == LZMA_MODE_FAST
			|| options->mode == LZMA_MODE_NORMAL
			|| options->mode == LZMA_MODE_ULTRA)
		&& options->near_depth > 0
		&& options->near_depth <= MATCH_CYCLES_MAX
		&& options->near_dict_size_log >= NEAR_DICT_LOG_MIN
		&& options->near_dict_size_log <= NEAR_DICT_LOG_MAX
		&& rmf_options_valid(options)
		&& options->threads > 0 && options->threads <= LZMA_THREADS_MAX;
}


extern lzma_ret
lzma_flzma2_encoder_init(lzma_next_coder *next,
		const lzma_allocator *allocator,
		const lzma_filter_info *filters)
{
	if (filters[0].options == NULL)
		return LZMA_PROG_ERROR;

	const lzma_options_lzma *options = filters[0].options;
	if (!is_options_valid(options))
		return LZMA_OPTIONS_ERROR;

	lzma2_fast_coder *coder = next->coder;
	if (coder == NULL) {
		coder = lzma_alloc_zero(sizeof(lzma2_fast_coder), allocator);
		if (coder == NULL)
			return LZMA_MEM_ERROR;

		coder->dict_block.data = NULL;
		coder->match_table = NULL;
		coder->threads = NULL;

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
	// Radix match-finder only searches to an even-numbered depth.
	coder->opt_cur.depth &= ~1;

	return_if_error(lzma2_fast_encoder_create(coder, allocator));

	coder->ending = false;
	coder->progress_in = 0;
	coder->progress_out = 0;
	coder->total_in = 0;
	coder->total_out = 0;

	// Initialize the next filter in the chain, if any.
	return lzma_next_filter_init(&coder->next, allocator, filters + 1);
}


extern uint64_t
lzma_flzma2_encoder_memusage(const void *options)
{
	const lzma_options_lzma *const opt = options;
	return rmf_memory_usage(opt->dict_size, opt->threads)
		+ lzma2_enc_rmf_mem_usage(opt->near_dict_size_log, opt->mode, opt->threads);
}
