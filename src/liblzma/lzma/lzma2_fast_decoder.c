///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma2_fast_decoder.c
/// \brief      Fast LZMA2 decoder wrapper
///
//  Authors:    Conor McCarthy
//              Lasse Collin
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "lzma2_fast_encoder.h" // flzma2_translate_error
#include "lzma2_fast_decoder.h"
#include "fast-lzma2.h"


#define ret_translate_if_error(expr) \
do { \
	const size_t ret_ = (expr); \
	if (FL2_isError(ret_)) \
		return flzma2_translate_error(ret_); \
} while (0)


typedef struct
{
	lzma_options_lzma lzma_opt;
	uint8_t prop;
} lzma2_options;


typedef struct
{
	/// Fast LZMA2 decoder
	FL2_DStream *fds;

	/// Next coder in the chain
	lzma_next_coder next;

} flzma2_decoder;


static lzma_ret
flzma2_decode(void *coder_ptr,
		const lzma_allocator *allocator lzma_attribute((__unused__)),
		const uint8_t *restrict in, size_t *restrict in_pos,
		size_t in_size, uint8_t *restrict out,
		size_t *restrict out_pos, size_t out_size,
		lzma_action action lzma_attribute((__unused__)))
{
	flzma2_decoder *coder = coder_ptr;

	FL2_outBuffer outbuf = { out, out_size, *out_pos };
	FL2_inBuffer inbuf = { in, in_size, *in_pos };

	size_t res = FL2_decompressStream(coder->fds, &outbuf, &inbuf);

	ret_translate_if_error(res);

	*in_pos = inbuf.pos;
	*out_pos = outbuf.pos;

	return (res == 0) ? LZMA_STREAM_END : LZMA_OK;
}


static void
flzma2_decoder_end(void *coder_ptr, const lzma_allocator *allocator)
{
	flzma2_decoder *coder = coder_ptr;

	lzma_next_end(&coder->next, allocator);

	FL2_freeDStream(coder->fds);

	lzma_free(coder, allocator);
}


extern lzma_ret
lzma_flzma2_decoder_init(lzma_next_coder *next, const lzma_allocator *allocator,
	const lzma_filter_info *filters)
{
	// LZMA2 can only be the last filter in the chain. This is enforced
	// by the raw_decoder initialization.
	assert(filters[1].init == NULL);
 
	// Allocate the base structure if it isn't already allocated.
	flzma2_decoder *coder = next->coder;
	if (coder == NULL) {
		coder = lzma_alloc(sizeof(flzma2_decoder), allocator);
		if (coder == NULL)
			return LZMA_MEM_ERROR;

		coder->fds = NULL;

		next->coder = coder;
		next->code = &flzma2_decode;
		next->end = &flzma2_decoder_end;

		coder->next = LZMA_NEXT_CODER_INIT;
	}

	lzma2_options *opt = filters[0].options;

	if (coder->fds == NULL) {
		coder->fds = FL2_createDStream();
		if (coder->fds == NULL)
			return LZMA_MEM_ERROR;
	}

	ret_translate_if_error(FL2_initDStream_withProp(coder->fds, opt->prop));

	// Initialize the next filter in the chain, if any.
	return lzma_next_filter_init(&coder->next, allocator, filters + 1);
}


extern uint64_t
lzma_flzma2_decoder_memusage(const void *options)
{
	const lzma_options_lzma *opt = options;
	return FL2_estimateDStreamSize(opt->dict_size, 1);
}


extern lzma_ret
lzma_flzma2_props_decode(void **options, const lzma_allocator *allocator,
	const uint8_t *props, size_t props_size)
{
	if (props_size != 1)
		return LZMA_OPTIONS_ERROR;

	// Check that reserved bits are unset.
	if (props[0] & 0xC0)
		return LZMA_OPTIONS_ERROR;

	// Decode the dictionary size.
	if (props[0] > 40)
		return LZMA_OPTIONS_ERROR;

	lzma2_options *opt = lzma_alloc(sizeof(lzma2_options), allocator);
	if (opt == NULL)
		return LZMA_MEM_ERROR;

	opt->lzma_opt.dict_size = FL2_getDictSizeFromProp(props[0]);
	opt->prop = props[0];

	*options = opt;

	return LZMA_OK;
}
