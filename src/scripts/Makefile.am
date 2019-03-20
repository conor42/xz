##
## Author: Lasse Collin
##
## This file has been put into the public domain.
## You can do whatever you want with this file.
##

nodist_bin_SCRIPTS = fxzdiff fxzgrep fxzmore fxzless
dist_man_MANS = fxzdiff.1 fxzgrep.1 fxzmore.1 fxzless.1

links = \
	fxzdiff-fxzcmp \
	fxzgrep-fxzegrep \
	fxzgrep-fxzfgrep

if COND_LZMALINKS
links += \
	fxzdiff-flzdiff \
	fxzdiff-flzcmp \
	fxzgrep-flzgrep \
	fxzgrep-flzegrep \
	fxzgrep-flzfgrep \
	fxzmore-flzmore \
	fxzless-flzless
endif

install-exec-hook:
	cd $(DESTDIR)$(bindir) && \
	for pair in $(links); do \
		target=`echo $$pair | sed 's/-.*$$//' | sed '$(transform)'` && \
		link=`echo $$pair | sed 's/^.*-//' | sed '$(transform)'` && \
		rm -f $$link && \
		$(LN_S) $$target $$link; \
	done

install-data-hook:
	cd $(DESTDIR)$(mandir)/man1 && \
	for pair in $(links); do \
		target=`echo $$pair | sed 's/-.*$$//' | sed '$(transform)'` && \
		link=`echo $$pair | sed 's/^.*-//' | sed '$(transform)'` && \
		rm -f $$link.1 && \
		$(LN_S) $$target.1 $$link.1; \
	done

uninstall-hook:
	cd $(DESTDIR)$(bindir) && \
	for pair in $(links); do \
		link=`echo $$pair | sed 's/^.*-//' | sed '$(transform)'` && \
		rm -f $$link; \
	done
	cd $(DESTDIR)$(mandir)/man1 && \
	for pair in $(links); do \
		link=`echo $$pair | sed 's/^.*-//' | sed '$(transform)'` && \
		rm -f $$link.1; \
	done
