/* Memory mapping, POSIX.                                        SESSION 10 */

#include "lr_platform.h"

struct lr_mapping {
	void *addr;
	size_t len;
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
