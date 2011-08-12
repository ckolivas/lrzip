#include <liblrzip_private.h>

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

/* needed for CRC routines */
#include "lzma/C/7zCrc.h"
#include "util.h"
#include "lrzip.h"
#include "rzip.h"

bool lrzip_init(void)
{
	/* generate crc table */
	CrcGenerateTable();
	return true;
}

void lrzip_config_env(Lrzip *lr)
{
	const char *eptr;
	/* Get Preloaded Defaults from lrzip.conf
	 * Look in ., $HOME/.lrzip/, /etc/lrzip.
	 * If LRZIP=NOCONFIG is set, then ignore config
	 */
	eptr = getenv("LRZIP");
	if (!eptr)
		read_config(lr->control);
	else if (!strstr(eptr,"NOCONFIG"))
		read_config(lr->control);
}

void lrzip_free(Lrzip *lr)
{
	if (!lr) return;
	rzip_control_free(lr->control);
	for (; lr->infiles && *lr->infiles; lr->infiles++)
		free(*lr->infiles);
	free(lr->infiles);
	free(lr->outfile);
	free(lr);
}

Lrzip *lrzip_new(Lrzip_Mode mode)
{
	Lrzip *lr;

	lr = calloc(1, sizeof(Lrzip));
	if (unlikely(!lr)) return NULL;
	lr->control = calloc(1, sizeof(rzip_control));
	if (unlikely(!lr->control)) goto error;
	if (unlikely(!initialize_control(lr->control))) goto error;
	lr->mode = mode;
	lr->control->library_mode = 1;
#define MODE_CHECK(X) \
	case LRZIP_MODE_##X: \
	lr->control->flags |= FLAG_##X; \
	break

	switch (mode) {
	MODE_CHECK(DECOMPRESS);
	MODE_CHECK(NO_COMPRESS);
	MODE_CHECK(LZO_COMPRESS);
	MODE_CHECK(BZIP2_COMPRESS);
	MODE_CHECK(ZLIB_COMPRESS);
	MODE_CHECK(ZPAQ_COMPRESS);
#undef MODE_CHECK
	default:
		goto error;
	}
	setup_overhead(lr->control);
	return lr;
error:
	lrzip_free(lr);
	return NULL;
}

Lrzip_Mode lrzip_mode_get(Lrzip *lr)
{
	if (!lr) return LRZIP_MODE_NONE;
	return lr->mode;
}

char **lrzip_files_get(Lrzip *lr)
{
	if (!lr) return NULL;
	return lr->infiles;
}

bool lrzip_file_add(Lrzip *lr, const char *file)
{
	if ((!lr) || (!file) || (!file[0])) return false;

	if (!lr->infile_buckets) {
		/* no files added */
		lr->infiles = calloc(INFILE_BUCKET_SIZE + 1, sizeof(char*));
		lr->infile_buckets++;
	} else if (lr->infile_idx == INFILE_BUCKET_SIZE * lr->infile_buckets + 1) {
		/* all buckets full, create new bucket */
		char **tmp;

		tmp = realloc(lr->infiles, ++lr->infile_buckets * INFILE_BUCKET_SIZE + 1);
		if (!tmp) return false;
		lr->infiles = tmp;
	}

	lr->infiles[lr->infile_idx++] = strdup(file);
	return true;
}

bool lrzip_file_del(Lrzip *lr, const char *file)
{
	size_t x;
	
	if ((!lr) || (!file) || (!file[0])) return false;
	if (!lr->infile_buckets) return true;

	for (x = 0; x <= lr->infile_idx + 1; x++) {
		if (!lr->infiles[x]) return true; /* not found */
		if (strcmp(lr->infiles[x], file)) continue; /* not a match */
		free(lr->infiles[x]);
		break;
	}
	/* update index */
	for (; x < lr->infile_idx; x++)
		lr->infiles[x] = lr->infiles[x] + 1;
	lr->infile_idx--;
	return true;
}

void lrzip_outfile_set(Lrzip *lr, const char *file)
{
	if ((!lr) || (!file) || (!file[0])) return;
	if (lr->outfile && (!strcmp(lr->outfile, file))) return;
	free(lr->outfile);
	lr->outfile = strdup(file);
	lr->control->outname = lr->outfile;
}

const char *lrzip_outfile_get(Lrzip *lr)
{
	if (!lr) return NULL;
	return lr->outfile;
}

bool lrzip_run(Lrzip *lr)
{
	struct timeval start_time, end_time;
	rzip_control *control;
	double seconds,total_time; // for timers
	size_t i;
	int hours,minutes;

	if (!lr) return false;
	control = lr->control;
	/* One extra iteration for the case of no parameters means we will default to stdin/out */
	for (i = 0; i < lr->infile_idx; i++) {
		lr->control->infile = lr->infiles[i];
		if (lr->control->infile) {
			if ((strcmp(lr->control->infile, "-") == 0))
				lr->control->flags |= FLAG_STDIN;
			else {
				struct stat infile_stat;

				stat(lr->control->infile, &infile_stat);
				if (unlikely(S_ISDIR(infile_stat.st_mode))) {
					print_err("lrzip only works directly on FILES.\n"
					"Use lrztar or pipe through tar for compressing directories.\n");
					return false;
				}
			}
		}

		if (lr->control->outname && (strcmp(lr->control->outname, "-") == 0)) {
			lr->control->flags |= FLAG_STDOUT;
			lr->control->msgout = stderr;
			register_outputfile(lr->control, lr->control->msgout);
		}

		/* If no output filename is specified, and we're using stdin,
		 * use stdout */
		if (!lr->control->outname && STDIN) {
			lr->control->flags |= FLAG_STDOUT;
			lr->control->msgout = stderr;
			register_outputfile(lr->control, lr->control->msgout);
		}

		if (!STDOUT) {
			lr->control->msgout = stdout;
			register_outputfile(lr->control, lr->control->msgout);
		}

		if (CHECK_FILE) {
			if (!DECOMPRESS) {
				print_err("Can only check file written on decompression.\n");
				lr->control->flags &= ~FLAG_CHECK;
			} else if (STDOUT) {
				print_err("Can't check file written when writing to stdout. Checking disabled.\n");
				lr->control->flags &= ~FLAG_CHECK;
			}
		}

		setup_ram(lr->control);

		gettimeofday(&start_time, NULL);

		if (unlikely(ENCRYPT && (!lr->control->pass_cb))) {
			print_err("No password callback set!\n");
			return false;
		}

		if (DECOMPRESS || TEST_ONLY)
			decompress_file(lr->control);
		else if (INFO)
			get_fileinfo(lr->control);
		else
			compress_file(lr->control);

		/* compute total time */
		gettimeofday(&end_time, NULL);
		total_time = (end_time.tv_sec + (double)end_time.tv_usec / 1000000) -
			      (start_time.tv_sec + (double)start_time.tv_usec / 1000000);
		hours = (int)total_time / 3600;
		minutes = (int)(total_time / 60) % 60;
		seconds = total_time - hours * 3600 - minutes * 60;
		if (!INFO)
			print_progress("Total time: %02d:%02d:%05.2f\n", hours, minutes, seconds);
	}
	return true;
}
