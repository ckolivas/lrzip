MAINTAINERCLEANFILES = Makefile.in lrunzip.1 lrztar.1 lrzuntar.1

man1_MANS = lrzip.1 lrunzip.1 lrzcat.1 lrztar.1 lrzuntar.1
man5_MANS = lrzip.conf.5

BUILT_SOURCES = lrunzip.1 lrzcat.1 lrztar.1 lrzuntar.1
CLEANFILES = $(BUILT_SOURCES)

EXTRA_DIST = lrzip.1 lrunzip.1.pod lrzcat.1.pod lrztar.1.pod lrzuntar.1.pod $(man5_MANS)

lrunzip.1: lrunzip.1.pod
	pod2man $< $@

lrzcat.1: lrzcat.1.pod
	pod2man $< $@

lrztar.1: lrztar.1.pod
	pod2man $< $@

lrzuntar.1: lrzuntar.1.pod
	pod2man $< $@
