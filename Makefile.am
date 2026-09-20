ACLOCAL_AMFLAGS = -I m4
MAINTAINERCLEANFILES = \
Makefile.in \
aclocal.m4 \
config.guess \
config.h.in \
config.h.in~ \
config.sub \
configure \
depcomp \
install-sh \
ltconfig \
ltmain.sh \
missing \
$(PACKAGE_TARNAME)-$(PACKAGE_VERSION).tar.gz \
$(PACKAGE_TARNAME)-$(PACKAGE_VERSION).tar.bz2 \
$(PACKAGE_TARNAME)-$(PACKAGE_VERSION).tar.xz \
$(PACKAGE_TARNAME)-$(PACKAGE_VERSION).tar.lrz \
$(PACKAGE_TARNAME)-$(PACKAGE_VERSION)-doc.tar.bz2 \
m4/libtool.m4 \
m4/lt~obsolete.m4 \
m4/ltoptions.m4 \
m4/ltsugar.m4 \
m4/ltversion.m4

SUBDIRS = lzma man doc

AM_CFLAGS = -I. -I lzma/C -DNDEBUG
AM_CXXFLAGS = $(AM_CFLAGS)

lrztardir = $(bindir)
lrztar_SCRIPTS = lrztar

noinst_LTLIBRARIES = libtmplrzip.la
libtmplrzip_la_SOURCES = \
  lrzip_private.h \
  lrzip.c \
  lrzip_core.h \
  rzip.h \
  match.h \
  rzip.c \
  runzip.c \
  runzip.h \
  stream.c \
  stream.h \
  filters.c \
  filters.h \
  util.c \
  util.h \
  md5.c \
  md5.h \
  aes.c \
  aes.h \
  gcm.c \
  gcm.h \
  sha4.c \
  sha4.h \
  libzpaq/libzpaq.cpp \
  libzpaq/libzpaq.h \
  libzpaq/zpaq_lrzip.cpp

libtmplrzip_la_LIBADD = lzma/C/liblzma.la


bin_PROGRAMS = lrzip
lrzip_SOURCES = \
  main.c
nodist_EXTRA_lrzip_SOURCES = dummyy.cxx

lrzip_LDADD = libtmplrzip.la
if STATIC
  lrzip_LDFLAGS = -all-static
endif

dist_doc_DATA = \
  AUTHORS \
  BUGS \
  ChangeLog \
  COPYING \
  README.md \
  README-NOT-BACKWARD-COMPATIBLE \
  TODO \
  WHATS-NEW

lrzipdir = $(includedir)

EXTRA_DIST = \
		lrztar \
		description-pak \
		autogen.sh \
		INSTALL \
		tests/regression.sh \
		tests/regression.good \
		tests/roundtrip_suite.sh \
		tests/ramout.sh \
		tests/checksum.sh \
		tests/sliding.sh \
		tests/recursive.sh \
		$(dist_doc_DATA)

check_PROGRAMS = tests/match_test tests/gcm_test tests/kdf_test tests/aes_test tests/sliding_test tests/tmpdir_test tests/dirlist_test
tests_dirlist_test_SOURCES = tests/dirlist_test.c
nodist_EXTRA_tests_dirlist_test_SOURCES = dummyy.cxx
tests_dirlist_test_LDADD = libtmplrzip.la
tests_tmpdir_test_SOURCES = tests/tmpdir_test.c
nodist_EXTRA_tests_tmpdir_test_SOURCES = dummyy.cxx
tests_tmpdir_test_LDADD = libtmplrzip.la
tests_sliding_test_SOURCES = tests/sliding_test.c
nodist_EXTRA_tests_sliding_test_SOURCES = dummyy.cxx
tests_sliding_test_LDADD = libtmplrzip.la
tests_match_test_SOURCES = tests/match_test.c
tests_gcm_test_SOURCES = tests/gcm_test.c
tests_gcm_test_LDADD = libtmplrzip.la
tests_aes_test_SOURCES = tests/aes_test.c
tests_aes_test_LDADD = libtmplrzip.la
tests_kdf_test_SOURCES = tests/kdf_test.c
nodist_EXTRA_tests_kdf_test_SOURCES = dummyy.cxx
tests_kdf_test_LDADD = libtmplrzip.la

# Match boundaries, then the combined gold + round-trip regression suite.
check-local: lrzip$(EXEEXT) $(check_PROGRAMS)
	$(AM_V_at)./tests/dirlist_test$(EXEEXT)
	$(AM_V_at)./tests/tmpdir_test$(EXEEXT)
	$(AM_V_at)LRZIP='$(abs_builddir)/lrzip$(EXEEXT)' \
		$(SHELL) $(srcdir)/tests/recursive.sh
	$(AM_V_at)./tests/sliding_test$(EXEEXT)
	$(AM_V_at)./tests/match_test$(EXEEXT)
	$(AM_V_at)./tests/gcm_test$(EXEEXT)
	$(AM_V_at)./tests/aes_test$(EXEEXT)
	$(AM_V_at)./tests/kdf_test$(EXEEXT)
	$(AM_V_at)LRZIP='$(abs_builddir)/lrzip$(EXEEXT)' \
		$(srcdir)/tests/regression.sh
	$(AM_V_at)LRZIP='$(abs_builddir)/lrzip$(EXEEXT)' \
		$(SHELL) $(srcdir)/tests/ramout.sh
	$(AM_V_at)LRZIP='$(abs_builddir)/lrzip$(EXEEXT)' \
		$(SHELL) $(srcdir)/tests/checksum.sh
	$(AM_V_at)LRZIP='$(abs_builddir)/lrzip$(EXEEXT)' \
		$(SHELL) $(srcdir)/tests/sliding.sh

install-exec-hook:
	$(LN_S) -f lrzip$(EXEEXT) $(DESTDIR)$(bindir)/lrunzip$(EXEEXT)
	$(LN_S) -f lrzip$(EXEEXT) $(DESTDIR)$(bindir)/lrzcat$(EXEEXT)
	$(LN_S) -f lrztar$(EXEEXT) $(DESTDIR)$(bindir)/lrzuntar$(EXEEXT)
	$(LN_S) -f lrzip$(EXEEXT) $(DESTDIR)$(bindir)/lrz$(EXEEXT)


uninstall-local:
	rm -f $(bindir)/lrunzip
	rm -f $(bindir)/lrzcat
	rm -f $(bindir)/lrzuntar
	rm -f $(bindir)/lrz

.PHONY: doc

# Documentation

doc: all
	@echo "entering doc/"
	$(MAKE) -C doc doc
