/*
   <windows.h>, with the guards it needs, for the win32 backend only.

   Kept separate from lr_platform_win32.h so that the Win32 API reaches the six
   files in this directory and nowhere else. See lr_platform_win32.h for why
   that containment matters.

   WIN32_LEAN_AND_MEAN drops winsock 1.1, RPC, OLE and shell headers.
   NOMINMAX drops the min/max macros, which collide with ordinary identifiers.
   NOGDI drops wingdi.h, whose ERROR macro is the classic collision.
*/

#ifndef LR_WIN32_H
#define LR_WIN32_H

/* Minimum Windows version this port targets. The Win32 headers hide APIs and
   their constants behind this: GetFinalPathNameByHandleW and the
   VOLUME_NAME_DOS / FILE_NAME_NORMALIZED flags it needs are Vista era, and
   MinGW's default target predates them. 0x0A00 is Windows 10, which is what
   the project supports and what UCRT ships inside. */
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
# define WINVER 0x0A00
#endif

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif
#ifndef NOGDI
# define NOGDI
#endif

#include <windows.h>

#endif /* LR_WIN32_H */
