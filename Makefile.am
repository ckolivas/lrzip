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

# The platform layer. -I lets both the application ("lr_platform.h" from
# lrzip_private.h) and the backends ("lr_platform.h" from platform/*/*.c)
# name the master header the same way.
AM_CPPFLAGS = -I$(srcdir)/platform

noinst_HEADERS = \
  platform/lr_platform.h \
  platform/lr_platform_posix.h \
  platform/lr_platform_win32.h \
  platform/win32/lr_win32.h

lrztardir = $(bindir)
lrztar_SCRIPTS = lrztar

noinst_LTLIBRARIES = libtmplrzip.la
libtmplrzip_la_SOURCES = \
  lrzip_private.h \
  lrzip.c \
  lrzip_core.h \
  rzip.h \
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

# Exactly one backend is compiled. Both directories carry the same six
# filenames, so switching platforms switches a directory.
if LR_PLATFORM_WIN32
libtmplrzip_la_SOURCES += \
  platform/win32/lr_console.c \
  platform/win32/lr_fs.c \
  platform/win32/lr_io.c \
  platform/win32/lr_mmap.c \
  platform/win32/lr_proc.c \
  platform/win32/lr_signal.c
else
libtmplrzip_la_SOURCES += \
  platform/posix/lr_console.c \
  platform/posix/lr_fs.c \
  platform/posix/lr_io.c \
  platform/posix/lr_mmap.c \
  platform/posix/lr_proc.c \
  platform/posix/lr_signal.c
endif

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
		$(dist_doc_DATA)

# Combined gold + round-trip regression suite.
check-local: lrzip$(EXEEXT)
	$(AM_V_at)LRZIP='$(abs_builddir)/lrzip$(EXEEXT)' \
		$(srcdir)/tests/regression.sh

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

