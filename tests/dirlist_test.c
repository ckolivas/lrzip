/* Exercise recursive list boundaries without creating half a million files. */
#include "lrzip_private.h"
#include "util.h"
#include <dirent.h>
#include <setjmp.h>
#include <sys/mman.h>

static jmp_buf failure_env;
static const char *failure_message;
static size_t allocation_size, stat_calls;
static bool fail_allocation, returned_entry;
static char *list;
static size_t entries;

static void require(bool ok)
{
	if (!ok) {
		fputs("Recursive directory list test failed\n", stderr);
		exit(1);
	}
}

static void test_failure(const char *format, ...)
{
	failure_message = format;
	longjmp(failure_env, 1);
}

static DIR *test_opendir(const char *path)
{
	(void)path;
	returned_entry = false;
	return (DIR *)&returned_entry;
}

static struct dirent *test_readdir(DIR *dir)
{
	static struct dirent entry;

	(void)dir;
	if (returned_entry)
		return NULL;
	returned_entry = true;
	strcpy(entry.d_name, "file");
	return &entry;
}

static int test_closedir(DIR *dir)
{
	(void)dir;
	return 0;
}

static int test_stat(const char *path, struct stat *st)
{
	(void)path;
	stat_calls++;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG;
	return 0;
}

static void *test_realloc(void *ptr, size_t size)
{
	void *result;

	(void)ptr;
	allocation_size = size;
	if (fail_allocation)
		return NULL;
	/* Only the final pathname's page is touched, even at the 2 GiB boundary. */
	result = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	require(result != MAP_FAILED);
	return result;
}

#undef failure
#define failure(...) test_failure(__VA_ARGS__)
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#define stat(...) test_stat(__VA_ARGS__)
#define realloc test_realloc
#define main lrzip_main
#include "../main.c"
#undef main
#undef realloc
#undef stat
#undef closedir
#undef readdir
#undef opendir

static void check_append(size_t start)
{
	entries = start;
	list = NULL;
	fail_allocation = false;
	if (setjmp(failure_env))
		require(false);
	recurse_dirlist("dir", &list, &entries);
	require(entries == start + 1);
	require(allocation_size == (start + 1) * (size_t)MAX_PATH_LEN);
	require(!strcmp(list + start * MAX_PATH_LEN, "dir/file"));
	require(munmap(list, allocation_size) == 0);
}

static void check_failure(size_t start, char *path, const char *message,
			  size_t expected_size, size_t expected_stats)
{
	char previous[] = "previous allocation";

	entries = start;
	list = previous;
	allocation_size = stat_calls = 0;
	fail_allocation = true;
	failure_message = NULL;
	if (!setjmp(failure_env)) {
		recurse_dirlist(path, &list, &entries);
		require(false);
	}
	require(failure_message && strstr(failure_message, message));
	require(entries == start && list == previous);
	require(!strcmp(previous, "previous allocation"));
	require(allocation_size == expected_size && stat_calls == expected_stats);
}

int main(void)
{
	char path[MAX_PATH_LEN];

	control = &base_control;
	control->msgout = control->msgerr = stderr;
	check_append(0);
	check_append(524287);
	check_append(524288);
	check_failure(0, "dir", "Unable to grow", MAX_PATH_LEN, 1);
	check_failure(524287, "dir", "Unable to grow", (size_t)1 << 31, 1);
	check_failure(SIZE_MAX / MAX_PATH_LEN, "dir", "too large", 0, 1);
	check_failure(SIZE_MAX, "dir", "too large", 0, 1);
	memset(path, 'a', sizeof(path));
	/* /file plus the terminator exactly fits, then exceeds the slot by one. */
	path[MAX_PATH_LEN - 6] = '\0';
	check_failure(0, path, "Unable to grow", MAX_PATH_LEN, 1);
	path[MAX_PATH_LEN - 6] = 'a';
	path[MAX_PATH_LEN - 5] = '\0';
	check_failure(0, path, "Path is too long", 0, 0);
	puts("Recursive directory list boundary and allocation failure tests passed");
	return 0;
}
