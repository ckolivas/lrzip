/* Filesystem queries, Win32.                            SESSION 6, SESSION 9 */

#include "lr_platform.h"
#include "lr_win32.h"

/* SESSION 6
   Windows has no fstatvfs. GetDiskFreeSpaceEx is path based, so the path has
   to be recovered from the descriptor:

       fd -> HANDLE            _get_osfhandle
       HANDLE -> full path     GetFinalPathNameByHandleW
       path -> mount root      GetVolumePathNameW
       root -> free bytes      GetDiskFreeSpaceExW

   GetVolumePathNameW rather than just taking the drive letter, because a
   volume can be mounted on a directory instead of a letter, and free space
   belongs to the volume, not to the leading "C:".

   Paths are wide throughout and sized past MAX_PATH: GetFinalPathNameByHandleW
   returns the \\?\ form, which is not bounded by MAX_PATH. */
lr_i64 lr_free_space(int fd)
{
	wchar_t path[4096], root[4096];
	const DWORD path_cch = (DWORD)(sizeof(path) / sizeof(path[0]));
	const DWORD root_cch = (DWORD)(sizeof(root) / sizeof(root[0]));
	ULARGE_INTEGER avail;
	HANDLE h;
	DWORD len;

	h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE)
		return -1;

	len = GetFinalPathNameByHandleW(h, path, path_cch,
					FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (len == 0 || len >= path_cch)
		return -1;

	if (!GetVolumePathNameW(path, root, root_cch))
		return -1;

	if (!GetDiskFreeSpaceExW(root, &avail, NULL, NULL))
		return -1;

	/* Caller compares against a signed i64 file size; saturate rather than
	   wrap on a volume larger than 8 EiB. */
	if (avail.QuadPart > (ULONGLONG)INT64_MAX)
		return INT64_MAX;

	return (lr_i64)avail.QuadPart;
}

/* SESSION 9 */
bool lr_set_file_time(const char *path, time_t mtime)
{
	(void)path;
	(void)mtime;
	return false;
}
