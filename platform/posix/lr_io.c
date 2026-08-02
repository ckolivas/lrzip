/* Positional I/O, POSIX.                                         SESSION 9 */

#include "lr_platform.h"
#include <errno.h>

ssize_t lr_pread(int fd, void *buf, size_t count, lr_i64 offset)
{
	(void)fd; (void)buf; (void)count; (void)offset;
	errno = ENOSYS;
	return -1;
}

ssize_t lr_pwrite(int fd, const void *buf, size_t count, lr_i64 offset)
{
	(void)fd; (void)buf; (void)count; (void)offset;
	errno = ENOSYS;
	return -1;
}
