##
## Author: Conor McCarthy
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

if COND_ENCODER_LZMA2
libflzma_la_SOURCES += \
    radix/radix_bitpack.c \
    radix/radix_engine.h \
	radix/radix_get.h \
    radix/radix_internal.h \
    radix/radix_mf.c \
    radix/radix_mf.h \
    radix/radix_struct.c
endif
