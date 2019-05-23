##
## Authors: Lasse Collin
##          Conor McCarthy
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

EXTRA_DIST += rangecoder/price_tablegen.c

libflzma_la_SOURCES += rangecoder/range_common.h

if COND_ENCODER_LZMA1
libflzma_la_SOURCES += \
	rangecoder/range_encoder.h \
	rangecoder/price.h \
	rangecoder/price_table.c
endif

if COND_ENCODER_LZMA2
libflzma_la_SOURCES += \
	rangecoder/range_fast_enc.h \
	rangecoder/range_fast_enc.c
endif

if COND_DECODER_LZMA1
libflzma_la_SOURCES += rangecoder/range_decoder.h
endif
