##
## Author: Conor McCarthy
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

libflzma_la_SOURCES += \
    fast-lzma2/data_block.h

if COND_ENCODER_LZMA2
libflzma_la_SOURCES += \
    fast-lzma2/lzma2_enc.c \
    fast-lzma2/lzma2_enc.h \
    fast-lzma2/radix_bitpack.c \
    fast-lzma2/radix_engine.h \
	fast-lzma2/radix_get.h \
    fast-lzma2/radix_internal.h \
    fast-lzma2/radix_mf.c \
    fast-lzma2/radix_mf.h \
    fast-lzma2/radix_struct.c \
    fast-lzma2/range_enc.c \
    fast-lzma2/range_enc.h
endif

if COND_DECODER_LZMA2
if COND_ASM_X86_64
libflzma_la_SOURCES += fast-lzma2/lzma_dec_x86_64.S
endif
endif
