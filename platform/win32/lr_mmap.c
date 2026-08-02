/* Memory mapping, Win32.                                        SESSION 10 */

#include "lr_platform.h"
#include "lr_win32.h"

/* The two fields POSIX has no equivalent of, and the reason lr_mapping is an
   opaque struct rather than a bare void *:

     mapping   CreateFileMapping's HANDLE, which must outlive the view and be
               closed separately from unmapping it.
     delta     Windows requires the mapping offset to be a multiple of
               dwAllocationGranularity (64 KiB), not merely page aligned. The
               offset is rounded down and the caller is handed a pointer moved
               forward by the difference, which has to be remembered in order
               to unmap the real base later. */
struct lr_mapping {
	HANDLE mapping;
	void *base;
	void *addr;
	size_t len;
	size_t delta;
	int fd;
	lr_i64 offset;
	bool writable;
};

lr_mapping *lr_map_create(int fd, lr_i64 offset, size_t len, bool writable)
{
	(void)fd; (void)offset; (void)len; (void)writable;
	return NULL;
}

void *lr_map_ptr(lr_mapping *m)
{
	(void)m;
	return NULL;
}

size_t lr_map_len(lr_mapping *m)
{
	(void)m;
	return 0;
}

bool lr_map_resize(lr_mapping *m, size_t new_len)
{
	(void)m; (void)new_len;
	return false;
}

void lr_map_destroy(lr_mapping *m)
{
	(void)m;
}
