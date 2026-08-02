/*
   Windows system headers, centralised. The counterpart to lr_platform_posix.h.

   Included only by lr_platform.h -- never directly.

   NOTE: this header deliberately does NOT include <windows.h>.

   lr_platform.h reaches every translation unit in lrzip, so anything included
   here is included everywhere. <windows.h> defines a large number of short,
   unprefixed macros (ERROR, IN, OUT, small, near, interface, ...) and pushing
   that into all of lrzip's algorithm code invites collisions that would have
   to be diagnosed one at a time.

   Only the six files under platform/win32/ actually need the Win32 API, and
   they include "lr_win32.h" for it. Application code needs the MinGW system
   headers below and nothing more.

   MinGW-w64 under UCRT64 supplies the following natively, so they need no
   emulation: dirent.h, getopt.h, libgen.h, utime.h, sys/time.h, unistd.h,
   signal.h, fcntl.h, sys/stat.h. It does NOT supply termios.h, sys/statvfs.h,
   sys/mman.h or arpa/inet.h -- those four are what this port must answer for.
*/

#ifndef LR_PLATFORM_WIN32_H
#define LR_PLATFORM_WIN32_H

#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>
#include <signal.h>

/* Not here, to stay symmetrical with lr_platform_posix.h: <dirent.h>,
   <getopt.h> and <libgen.h>. MinGW supplies all three and main.c includes
   them directly, so there is nothing for this layer to abstract. */

/* <arpa/inet.h> replacement.

   lrzip uses exactly one function from that header -- ntohl(), twice, in
   lrzip.c, decoding a field of the archive header. Windows offers it in
   <winsock2.h>, but including winsock everywhere and linking ws2_32 to obtain
   one byte swap is a poor trade. Windows runs only on little-endian hardware,
   so network-to-host order is unconditionally a 32-bit swap. */
#ifndef ntohl
# define ntohl(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#endif
#ifndef htonl
# define htonl(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#endif

/* No <sys/mman.h> substitute is offered here, on purpose.

   Memory mapping is Session 10 and is a semantic problem, not a naming one.
   Leaving the header out means the mmap/munmap/mremap/mlock call sites fail as
   ordinary errors rather than as a fatal missing-header wall -- which is the
   point: the compiler can then enumerate every unresolved call in the file
   instead of stopping at line 37. */

#endif /* LR_PLATFORM_WIN32_H */
