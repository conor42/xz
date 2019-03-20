##
## Author: Conor McCarthy
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

libflzma_la_CPPFLAGS += -DNO_XXHASH

libflzma_la_SOURCES += \
    fast-lzma2/compiler.h \
    fast-lzma2/data_block.h \
    fast-lzma2/fast-lzma2.h \
    fast-lzma2/fl2_common.c \
    fast-lzma2/fl2_errors.h \
    fast-lzma2/fl2_internal.h \
    fast-lzma2/fl2_pool.c \
    fast-lzma2/fl2_pool.h \
    fast-lzma2/fl2_threading.c \
    fast-lzma2/fl2_threading.h \
    fast-lzma2/mem.h \
    fast-lzma2/platform.h \
    fast-lzma2/util.c \
    fast-lzma2/util.h

if COND_ENCODER_LZMA2
libflzma_la_SOURCES += \
    fast-lzma2/atomic.h \
    fast-lzma2/count.h \
	fast-lzma2/dict_buffer.c \
	fast-lzma2/dict_buffer.h \
    fast-lzma2/fastpos_table.h \
    fast-lzma2/fl2_compress.c \
    fast-lzma2/fl2_compress_internal.h \
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
libflzma_la_CPPFLAGS += -DFL2_XZ_BUILD
endif

if COND_DECODER_LZMA2
libflzma_la_SOURCES += \
	fast-lzma2/fl2_decompress.c \
	fast-lzma2/lzma2_dec.c \
	fast-lzma2/lzma2_dec.h
if COND_ASM_X86_64
libflzma_la_SOURCES += fast-lzma2/lzma_dec_x86_64.S
libflzma_la_CPPFLAGS += -DLZMA2_DEC_OPT
if COND_W32
libflzma_la_CPPFLAGS += -DMS_x64_CALL=1
else
libflzma_la_CPPFLAGS += -DMS_x64_CALL=0
endif
endif
endif
