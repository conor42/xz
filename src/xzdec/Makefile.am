##
## Author: Lasse Collin
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

# Windows resource compiler support. It's fine to use fxz_CPPFLAGS
# also for flzmadec.
.rc.o:
	$(RC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
		$(fxzdec_CPPFLAGS) $(CPPFLAGS) $(RCFLAGS) -i $< -o $@


fxzdec_SOURCES = \
	xzdec.c \
	../common/tuklib_progname.c \
	../common/tuklib_exit.c

if COND_W32
fxzdec_SOURCES += xzdec_w32res.rc
endif

fxzdec_CPPFLAGS = \
	-DTUKLIB_GETTEXT=0 \
	-I$(top_srcdir)/src/common \
	-I$(top_srcdir)/src/liblzma/api \
	-I$(top_builddir)/lib
fxzdec_LDADD = $(top_builddir)/src/liblzma/libflzma.la

if COND_GNULIB
fxzdec_LDADD += $(top_builddir)/lib/libgnu.a
endif

fxzdec_LDADD += $(LTLIBINTL)


flzmadec_SOURCES = \
	xzdec.c \
	../common/tuklib_progname.c \
	../common/tuklib_exit.c

if COND_W32
flzmadec_SOURCES += lzmadec_w32res.rc
endif

flzmadec_CPPFLAGS = $(fxzdec_CPPFLAGS) -DLZMADEC
flzmadec_LDFLAGS = $(fxzdec_LDFLAGS)
flzmadec_LDADD = $(fxzdec_LDADD)


bin_PROGRAMS =

if COND_XZDEC
bin_PROGRAMS += fxzdec
dist_man_MANS = fxzdec.1
endif

if COND_LZMADEC
bin_PROGRAMS += flzmadec

# Create the symlink flzmadec.1->fxzdec.1 only if fxzdec.1 was installed.
# This is better than creating a dangling symlink, especially
# because creating the link may fail due to the directory being missing.
#
# FIXME: The correct solution would be to install fxzdec.1 as flzmadec.1
# but I don't know what is the sane way to do it and since this is a bit
# unusual situation anyway, it's not that important.
if COND_XZDEC
install-data-hook:
	cd $(DESTDIR)$(mandir)/man1 && \
	target=`echo fxzdec | sed '$(transform)'` && \
	link=`echo flzmadec | sed '$(transform)'` && \
	rm -f $$link.1 && \
	$(LN_S) $$target.1 $$link.1

uninstall-hook:
	cd $(DESTDIR)$(mandir)/man1 && \
	link=`echo flzmadec | sed '$(transform)'` && \
	rm -f $$link.1
endif
endif
