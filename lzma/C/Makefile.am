MAINTAINERCLEANFILES = Makefile.in

AM_CFLAGS = \
  -DCOMPRESS_MF_MT \
  -D_REENTRANT \
  -I@top_builddir@ \
  -I@top_srcdir@

ASM_LIBS =
ASM_S =
if USE_ASM
  ASM_LIBS += @ASM_OBJ@
else
  ASM_S += 7zCrc.c 7zCrc.h
endif

noinst_LTLIBRARIES = liblzma.la
liblzma_la_SOURCES = \
  $(ASM_S) \
	LzmaDec.h \
	LzmaEnc.h \
	LzFind.c \
	LzFind.h \
	LzFindMt.c \
	LzFindMt.h \
	LzmaDec.c \
	LzmaEnc.c \
	LzmaLib.c \
	LzmaLib.h \
	Alloc.c \
	Alloc.h \
	Threads.c \
	Threads.h \
	Types.h \
	LzHash.h \
	windows.h \
	basetyps.h \
	MyWindows.h \
	MyGuidDef.h
liblzma_so_LIBS = $(ASM_LIBS)

7zCrcT8U.o: @top_srcdir@/lzma/ASM/x86/7zCrcT8U.s
	@ASM_CMD@ -o 7zCrcT8U.o @top_srcdir@/lzma/ASM/x86/7zCrcT8U.s

7zCrcT8U_64.o: @top_srcdir@/lzma/ASM/x86_64/7zCrcT8U_64.s
	@ASM_CMD@ -o 7zCrcT8U_64.o @top_srcdir@/lzma/ASM/x86_64/7zCrcT8U_64.s
