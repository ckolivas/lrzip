dnl Process this file with autoconf to produce a configure script.
AC_INIT([lrzip],[0.561],[kernel@kolivas.org],[lrzip-0.561])
AC_CONFIG_HEADER(config.h)
# see what our system is!
AC_CANONICAL_HOST

AC_ARG_ENABLE(
		asm,
		[AC_HELP_STRING([--enable-asm],[Enable native Assembly code])],
		ASM=$enableval,
		ASM=yes
             )
if test x"$ASM" = xyes; then
	AC_CHECK_PROG( ASM_PROG, nasm, yes, no )
	if test x"$ASM_PROG" = x"no "; then
		ASM=no
	fi
fi

dnl Checks for programs.
AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_LN_S
AC_SUBST(SHELL)
AC_SYS_LARGEFILE

if test x"$GCC" = xyes; then
        CFLAGS="-Wall -W -s -O2 $CFLAGS"
        CXXFLAGS="-Wall -W -s -O2 $CXXFLAGS"
fi

AC_CHECK_HEADERS(fcntl.h sys/time.h sys/unistd.h unistd.h)
AC_CHECK_HEADERS(sys/param.h ctype.h sys/wait.h sys/ioctl.h)
AC_CHECK_HEADERS(string.h stdlib.h sys/types.h)

AC_TYPE_OFF_T
AC_TYPE_SIZE_T

AC_CHECK_SIZEOF(int)
AC_CHECK_SIZEOF(long)
AC_CHECK_SIZEOF(short)

AC_CACHE_CHECK([for large file support],rzip_cv_HAVE_LARGE_FILES,[
AC_RUN_IFELSE([AC_LANG_SOURCE([[
#include <stdio.h>
#include <sys/types.h>
main() { return (sizeof(off_t) == 4); }]])],[rzip_cv_HAVE_LARGE_FILES=yes],[rzip_cv_HAVE_LARGE_FILES=no],[rzip_cv_HAVE_LARGE_FILES=cross])])
if test x"$rzip_cv_HAVE_LARGE_FILES" = x"yes"; then
    AC_DEFINE(HAVE_LARGE_FILES, 1, [ ])
fi


AC_C_INLINE

AC_CHECK_LIB(pthread, pthread_create, ,
        AC_MSG_ERROR([Could not find pthread library - please install libpthread]))
AC_CHECK_LIB(m, sqrt, ,
        AC_MSG_ERROR([Could not find math library - please install libm]))
AC_CHECK_LIB(z, compress2, ,
	AC_MSG_ERROR([Could not find zlib library - please install zlib-dev]))
AC_CHECK_LIB(bz2, BZ2_bzBuffToBuffCompress, ,
        AC_MSG_ERROR([Could not find bz2 library - please install libbz2-dev]))
AC_CHECK_LIB(lzo2, lzo1x_1_compress, ,
        AC_MSG_ERROR([Could not find lzo2 library - please install liblzo2-dev]))

AC_DEFINE([HAVE_ERRNO_DECL],[0],[Define to 1 if errno.h present])

echo $ECHO_N "checking for errno in errno.h... $ECHO_C"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <errno.h>]], [[int i = errno]])],[echo yes; AC_DEFINE(HAVE_ERRNO_DECL)],[echo no])

AC_CHECK_FUNCS(mmap strerror)
AC_CHECK_FUNCS(getopt_long)

# final checks for x86 and/or assembler
if test x"$ASM" = x"no"; then
	ASM_OBJ=7zCrc.o
	ASM=
else
	case $host in
		i?86-*)
			ASM_OBJ="7zCrcT8.o 7zCrcT8U.o"
			ASM="nasm -f elf" ;;
# x86_64 code is broken still
#		x86_64-*)
#			ASM_OBJ="7zCrcT8.o 7zCrcT8U_64.o"
#			ASM="nasm -f elf64" ;;
		*) ASM_OBJ=7zCrc.o ;;
	esac
fi

AC_SUBST([ASM_OBJ])
AC_SUBST([ASM])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
