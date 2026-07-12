MAINTAINERCLEANFILES = Makefile.in

# LZMA SDK 26.02 (Igor Pavlov, public domain) — LzmaLib subset for lrzip.
# Multithreaded match finder needs pthreads (_REENTRANT).
# Pure -fsanitize=undefined builds (no ASan) should also pass
# -DLRZIP_PORTABLE_UNALIGNED so CpuArch uses byte-wise unaligned access.
AM_CFLAGS = \
  -D_REENTRANT \
  -D_FILE_OFFSET_BITS=64 \
  -D_LARGEFILE_SOURCE \
  -I@top_builddir@ \
  -I@top_srcdir@

noinst_LTLIBRARIES = liblzma.la

liblzma_la_SOURCES = \
	7zCrc.c \
	7zCrc.h \
	7zCrcOpt.c \
	Alloc.c \
	Alloc.h \
	Bra.h \
	Bra.c \
	Bra86.c \
	Compiler.h \
	Delta.c \
	Delta.h \
	CpuArch.c \
	CpuArch.h \
	LzFind.c \
	LzFind.h \
	LzFindMt.c \
	LzFindMt.h \
	LzFindOpt.c \
	LzHash.h \
	LzmaDec.c \
	LzmaDec.h \
	LzmaEnc.c \
	LzmaEnc.h \
	LzmaLib.c \
	LzmaLib.h \
	Precomp.h \
	RotateDefs.h \
	Threads.c \
	Threads.h \
	7zTypes.h \
	7zWindows.h
