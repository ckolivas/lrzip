/*
   POSIX system headers, centralised.

   These are the includes that used to sit at the top of lrzip.c, main.c,
   util.c, rzip.c and stream.c. Gathering them here is what lets application
   code stop naming OS headers: on POSIX every one of them still arrives,
   so behaviour is unchanged, while on Windows lr_platform_win32.h supplies
   a different set and the application does not notice.

   Included only by lr_platform.h -- never directly.
*/

#ifndef LR_PLATFORM_POSIX_H
#define LR_PLATFORM_POSIX_H

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <arpa/inet.h>
#include <utime.h>
#include <signal.h>

/* Deliberately NOT here: <dirent.h>, <getopt.h>, <libgen.h>. Both platforms
   supply all three, so main.c still includes them itself and there is nothing
   to abstract. Hoisting them would also be actively harmful: this header is
   reached before <string.h>, and on glibc including <libgen.h> first switches
   basename() from the GNU version to the POSIX one, which modifies its
   argument in place. That would be a silent behaviour change on Linux
   introduced by a Windows port. */

#endif /* LR_PLATFORM_POSIX_H */
