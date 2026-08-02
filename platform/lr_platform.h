/*
   Platform abstraction layer for lrzip.

   This is the only platform header application code includes. It is pulled in
   first from lrzip_private.h, which every application translation unit reaches
   directly or transitively, so it is the single point where the OS is decided.

   Two jobs:

     1. Include the system headers the application needs, per platform. This is
        why application code no longer includes <termios.h>, <sys/statvfs.h> and
        friends itself -- those includes moved here.

     2. Declare the lr_* interface. Interfaces are shaped by what callers need,
        not by what POSIX happens to expose: lr_free_space() returns one integer
        rather than reproducing struct statvfs, and lr_map_resize() is named for
        the caller's intent because Windows cannot offer mremap's mechanism.

   One implementation of each function exists per platform, in platform/posix/
   and platform/win32/. The filenames are parallel, so the build selects a
   directory rather than a file list.
*/

#ifndef LR_PLATFORM_H
#define LR_PLATFORM_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>

/* The application's own i64 lives in lrzip_private.h, which includes this
   header before defining it. The interface therefore needs its own name. */
typedef int64_t lr_i64;

#if defined(_WIN32)
# define LR_PLATFORM_WIN32 1
# include "lr_platform_win32.h"
#else
# define LR_PLATFORM_POSIX 1
# include "lr_platform_posix.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ console
   Toggle terminal echo around the passphrase prompt. Both return false if
   stdin is not a terminal, which callers may ignore -- there is no echo to
   suppress when input is a pipe or a file.                      SESSION 6 */
bool lr_echo_disable(void);
bool lr_echo_enable(void);

/* --------------------------------------------------------------- filesystem
   Bytes free to this user on the filesystem holding fd; -1 on error.

   Takes a descriptor rather than a path because both call sites hold an
   already-open output file and used fstatvfs(). Windows has no fstatvfs and
   its free-space call is path based, so the win32 side recovers a path from
   the handle -- the awkwardness belongs here, not at the call sites.
                                                                  SESSION 6 */
lr_i64 lr_free_space(int fd);

/* Copy a modification time onto a file.                          SESSION 9 */
bool lr_set_file_time(const char *path, time_t mtime);

/* ------------------------------------------------------------------- timing
   Monotonic-ish millisecond clock for progress reporting.        SESSION 9 */
lr_i64 lr_time_ms(void);

/* --------------------------------------------------------------- scheduling
   Drop this process to background priority ("nice").             SESSION 9 */
bool lr_set_low_priority(void);

/* ----------------------------------------------------------- positional I/O
   Read/write at an absolute offset without moving the file pointer.
                                                                  SESSION 9 */
ssize_t lr_pread(int fd, void *buf, size_t count, lr_i64 offset);
ssize_t lr_pwrite(int fd, const void *buf, size_t count, lr_i64 offset);

/* --------------------------------------------------------------- interrupts
   Install a Ctrl+C handler.

   Semantic difference that must not be papered over: on POSIX the handler runs
   on the interrupted thread in signal context, so only async-signal-safe calls
   are legal. On Windows it runs on a separate OS-created thread, so it must be
   thread-safe instead. Keep handlers minimal -- set a flag and return.
                                                                  SESSION 8 */
bool lr_install_interrupt_handler(void (*handler)(void));

/* ----------------------------------------------------------- memory mapping
   lr_mapping is opaque. This is the decision most expensive to get wrong: the
   win32 implementation must retain a HANDLE and an alignment delta that POSIX
   has no use for, and a bare void * return would have nowhere to keep them.

   lr_map_resize() replaces mremap(). Callers MUST re-fetch the pointer with
   lr_map_ptr() afterwards -- on Windows the region is unmapped and remapped,
   so the address moves.                                         SESSION 10 */
typedef struct lr_mapping lr_mapping;

lr_mapping *lr_map_create(int fd, lr_i64 offset, size_t len, bool writable);
void       *lr_map_ptr(lr_mapping *m);
size_t      lr_map_len(lr_mapping *m);
bool        lr_map_resize(lr_mapping *m, size_t new_len);
void        lr_map_destroy(lr_mapping *m);

#ifdef __cplusplus
}
#endif

#endif /* LR_PLATFORM_H */
