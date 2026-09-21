/* Pool changes must preserve the old wrap limit until all slots are drained. */
#include "lrzip_private.h"
#include "util.h"

static bool checking_drain;
static int drain_waits;
static void checked_wait(const rzip_control *control, cksem_t *sem);

#define cksem_wait checked_wait
#include "../stream.c"
#undef cksem_wait

static void require(bool ok)
{
	if (!ok) {
		fputs("Compression thread pool test failed\n", stderr);
		exit(1);
	}
}

static void checked_wait(const rzip_control *control, cksem_t *sem)
{
	if (checking_drain) {
		require(control->threads == 3 && drain_waits < 3);
		require(sem == &cthreads[(2 + drain_waits) % 3].cksem);
		drain_waits++;
	}
	cksem_wait(control, sem);
}

static void check_pool(i64 usable_ram, i64 chunk_limit, int expected_threads)
{
	rzip_control control = {0};
	struct stream_info *sinfo;
	int i;

	control.msgout = control.msgerr = stderr;
	control.threads = 2;
	control.page_size = 4096;
	control.overhead = 4 * 1024 * 1024;
	control.usable_ram = usable_ram;
	/* Simulate the cursors left by an earlier file's completed jobs. */
	output_thread = next_compress_thread = 2;
	require(prepare_streamout_threads(&control));
	require(control.threads == 3);
	require(output_thread == 0 && next_compress_thread == 0);

	/* A subsequent chunk can start with both cursors partway round. */
	output_thread = next_compress_thread = 2;
	drain_waits = 0;
	checking_drain = true;
	sinfo = open_stream_out(&control, -1, 2, chunk_limit, 4);
	checking_drain = false;
	require(sinfo != NULL && control.threads == expected_threads);
	if (expected_threads < 3) {
		require(drain_waits == 3);
		require(output_thread == 0 && next_compress_thread == 0);
	} else {
		require(drain_waits == 0);
		require(output_thread == 2 && next_compress_thread == 2);
	}
	for (i = 0; i < 2; i++)
		free(sinfo->s[i].buf);
	free(sinfo->s);
	free(sinfo);
	/* All three allocated semaphores are idle, including inactive slots. */
	for (i = 0; i < 3; i++) {
#ifdef __APPLE__
		close(cthreads[i].cksem.pipefd[0]);
		close(cthreads[i].cksem.pipefd[1]);
#else
		require(sem_destroy(&cthreads[i].cksem) == 0);
#endif
	}
	free(cthreads);
	free(control.pthreads);
}

int main(void)
{
	check_pool(28 * 1024 * 1024, 4096, 3);
	check_pool(28 * 1024 * 1024, 12 * 1024 * 1024, 2);
	check_pool(8 * 1024 * 1024, 12 * 1024 * 1024, 1);
	puts("Compression thread pool reset and reduction tests passed");
	return 0;
}
