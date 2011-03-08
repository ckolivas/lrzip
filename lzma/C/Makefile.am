MAINTAINERCLEANFILES = Makefile.in

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
		Alloc.h \
		basetyps.h \
		LzFind.h \
		LzFindMt.h \
		LzHash.h \
		LzmaDec.h \
		LzmaEnc.h \
		LzmaLib.h \
		MyGuidDef.h \
		MyWindows.h \
		Threads.h \
		Types.h \
		windows.h \
  Threads.c \
  LzFind.c \
  LzFindMt.c \
  LzmaDec.c \
  LzmaEnc.c \
  LzmaLib.c
liblzma_so_LIBS = $(ASM_LIBS)

liblzma_so_CFLAGS = \
  -DCOMPRESS_MF_MT \
  -D_REENTRANT \
  -I@top_builddir@

7zCrcT8U.o: @top_srcdir@/lzma/ASM/x86/7zCrcT8U.s
	@ASM_CMD@ -o 7zCrcT8U.o @top_srcdir@/lzma/ASM/x86/7zCrcT8U.s

7zCrcT8U_64.o: @top_srcdir@/lzma/ASM/x86_64/7zCrcT8U_64.s
	@ASM_CMD@ -o 7zCrcT8U_64.o @top_srcdir@/lzma/ASM/x86_64/7zCrcT8U_64.s
