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
$(PACKAGE_TARNAME)-$(PACKAGE_VERSION)-doc.tar.bz2 \
m4/libtool.m4 \
m4/lt~obsolete.m4 \
m4/ltoptions.m4 \
m4/ltsugar.m4 \
m4/ltversion.m4

SUBDIRS = lzma man doc

AM_CFLAGS = -I lzma/C -DNDEBUG

lrztardir = @bindir@
lrztar_SCRIPTS = lrztar

bin_PROGRAMS = lrzip
lrzip_SOURCES = \
  zpipe.cpp \
  zpipe.h \
  lrzip_private.h \
  liblrzip.h \
  lrzip.c \
  lrzip.h \
  main.c \
  rzip.h \
  rzip.c \
  runzip.c \
  runzip.h \
  stream.c \
  stream.h \
  util.c \
  util.h \
  md5.c \
  md5.h \
  aes.c \
  aes.h \
  sha4.c \
  sha4.h

lrzip_LDADD = lzma/C/liblzma.la
if STATIC
  lrzip_LDFLAGS = -all-static
endif

dist_doc_DATA = \
  AUTHORS \
  BUGS \
  ChangeLog \
  COPYING \
  README \
  README-NOT-BACKWARD-COMPATIBLE \
  TODO \
  WHATS-NEW

EXTRA_DIST = \
		lrztar \
		regression_test.sh \
		description-pak \
		autogen.sh \
		INSTALL \
		$(dist_doc_DATA)
