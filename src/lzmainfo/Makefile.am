##
## Author: Lasse Collin
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

bin_PROGRAMS = flzmainfo

flzmainfo_SOURCES = \
	lzmainfo.c \
	../common/tuklib_progname.c \
	../common/tuklib_exit.c

if COND_W32
flzmainfo_SOURCES += lzmainfo_w32res.rc
endif

flzmainfo_CPPFLAGS = \
	-DLOCALEDIR=\"$(localedir)\" \
	-I$(top_srcdir)/src/common \
	-I$(top_srcdir)/src/liblzma/api \
	-I$(top_builddir)/lib

flzmainfo_LDADD = $(top_builddir)/src/liblzma/libflzma.la

if COND_GNULIB
flzmainfo_LDADD += $(top_builddir)/lib/libgnu.a
endif

flzmainfo_LDADD += $(LTLIBINTL)


dist_man_MANS = flzmainfo.1

# Windows resource compiler support
.rc.o:
	$(RC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
		$(flzmainfo_CPPFLAGS) $(CPPFLAGS) $(RCFLAGS) -i $< -o $@
