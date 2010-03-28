# Makefile for
# lrzip. This is processed by configure to produce the final
# Makefile
# See README.Assembler for notes about ASM module.

prefix=/usr
exec_prefix=${prefix}
datarootdir=${prefix}/share
ASM_OBJ=7zCrc.o
PACKAGE_TARNAME=lrzip-0.44
INSTALL_BIN=$(exec_prefix)/bin
INSTALL_MAN1=/usr/share/man/man1
INSTALL_MAN5=/usr/share/man/man5
INSTALL_DOC=${datarootdir}/doc/${PACKAGE_TARNAME}
INSTALL_DOC_LZMA=${datarootdir}/doc/${PACKAGE_TARNAME}/lzma
LIBS=-llzo2 -lbz2 -lz -lm -lpthread 
LDFLAGS=
CC=gcc
CXX=g++
CFLAGS=-O2 -march=native -fomit-frame-pointer -I. -I$(srcdir) -Wall -W -c
CXXFLAGS=-O2 -march=native -fomit-frame-pointer -I. -I$(srcdir) -Wall -W -c
LZMA_CFLAGS=-I./lzma/C -DCOMPRESS_MF_MT -D_REENTRANT

INSTALLCMD=/usr/bin/install -c
LN_S=ln -s
RM=rm -f
ASM=yes


srcdir=.
SHELL=/bin/sh

.SUFFIXES:
.SUFFIXES: .c .o

OBJS= main.o rzip.o runzip.o stream.o util.o \
  7zCrc.o \
  zpipe.o \
  Threads.o \
  LzFind.o \
  LzFindMt.o \
  LzmaDec.o \
  LzmaEnc.o \
  LzmaLib.o

DOCFILES= AUTHORS BUGS ChangeLog COPYING README README-NOT-BACKWARD-COMPATIBLE \
	  TODO WHATS-NEW \
	  doc/README.Assembler doc/README.benchmarks \
	  doc/README.lzo_compresses.test.txt \
	  doc/magic.header.txt doc/lrzip.conf.example
DOCFILES_LZMA= lzma/7zC.txt lzma/7zFormat.txt lzma/Methods.txt \
	  lzma/history.txt lzma/lzma.txt lzma/README lzma/README-Alloc

MAN1FILES= man/lrzip.1 man/lrztar.1 man/lrunzip.1
MAN5FILES= man/lrzip.conf.5

#note that the -I. is needed to handle config.h when using VPATH
.c.o:
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) $< -o $@

all: lrzip man doc

7zCrcT8.o: ./lzma/C/7zCrcT8.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/7zCrcT8.c

7zCrcT8U.o: ./lzma/ASM/x86/7zCrcT8U.s
	$(ASM) -o 7zCrcT8U.o ./lzma/ASM/x86/7zCrcT8U.s

7zCrcT8U_64.o: ./lzma/ASM/x86_64/7zCrcT8U_64.s
	$(ASM) -o 7zCrcT8U_64.o ./lzma/ASM/x86_64/7zCrcT8U_64.s

7zCrc.o: ./lzma/C/7zCrc.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/7zCrc.c

LzmaLib.o: ./lzma/C/LzmaLib.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/LzmaLib.c

LzmaDec.o:  ./lzma/C/LzmaDec.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/LzmaDec.c

LzmaEnc.o: ./lzma/C/LzmaEnc.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/LzmaEnc.c

Threads.o: ./lzma/C/Threads.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/Threads.c

LzFind.o: ./lzma/C/LzFind.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/LzFind.c

LzFindMt.o: ./lzma/C/LzFindMt.c
	$(CC) $(CFLAGS) $(LZMA_CFLAGS) ./lzma/C/LzFindMt.c

zpipe.o: zpipe.cpp
	$(CXX) $(CXXFLAGS) -DNDEBUG zpipe.cpp

man: man/lrztar.1 man/lrunzip.1

man/lrztar.1: man/lrztar.1.pod
	$(MAKE) -f pod2man.mk PACKAGE=lrztar PODCENTER=lrzip makeman

man/lrunzip.1: man/lrunzip.1.pod
	$(MAKE) -f pod2man.mk PACKAGE=lrunzip PODCENTER=lrzip makeman

install: all
	mkdir -p $(DESTDIR)${INSTALL_BIN}
	${INSTALLCMD} -m 755 lrzip $(DESTDIR)${INSTALL_BIN}
	( cd $(DESTDIR)${INSTALL_BIN} && ${LN_S} -f lrzip lrunzip )
	${INSTALLCMD} -m 755 lrztar $(DESTDIR)${INSTALL_BIN}
	mkdir -p $(DESTDIR)${INSTALL_MAN1}
	${INSTALLCMD} -m 644 $(MAN1FILES) $(DESTDIR)${INSTALL_MAN1}
	mkdir -p $(DESTDIR)${INSTALL_MAN5}
	${INSTALLCMD} -m 644 $(MAN5FILES) $(DESTDIR)${INSTALL_MAN5}
	mkdir -p $(DESTDIR)${INSTALL_DOC}
	${INSTALLCMD} -m 644 $(DOCFILES) $(DESTDIR)${INSTALL_DOC}
	mkdir -p $(DESTDIR)${INSTALL_DOC_LZMA}
	${INSTALLCMD} -m 644 $(DOCFILES_LZMA) $(DESTDIR)${INSTALL_DOC_LZMA}

lrzip: $(OBJS)
	$(CXX) $(LDFLAGS) -o lrzip $(OBJS) $(LIBS)

static: $(OBJS)
	$(CXX) $(LDFLAGS) -static -o lrzip $(OBJS) $(LIBS)

clean:
	-${RM} *~ $(OBJS) lrzip config.cache config.log config.status *.o

