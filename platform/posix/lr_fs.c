/* Filesystem queries, POSIX.                            SESSION 6, SESSION 9 */

#include "lr_platform.h"

/* SESSION 6 */
lr_i64 lr_free_space(int fd)
{
	struct statvfs buf;

	if (fstatvfs(fd, &buf))
		return -1;

	/* f_bavail is blocks free to an unprivileged user, which is what the
	   caller wants to know -- f_bfree would include the root reserve. The
	   f_bsize multiplier matches what lrzip used before the port. */
	return (lr_i64)buf.f_bsize * (lr_i64)buf.f_bavail;
}

/* SESSION 9 */
bool lr_set_file_time(const char *path, time_t mtime)
{
	(void)path;
	(void)mtime;
	return false;
}
