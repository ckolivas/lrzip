#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <lrzip_private.h>
#include <Lrzip.h>

#define INFILE_BUCKET_SIZE 10

struct Lrzip
{
	Lrzip_Mode mode;
	unsigned int flags;
	rzip_control *control;

	/* bucket allocation is used here to avoid frequent calls to realloc */
	char **infilenames;
	size_t infilename_idx;
	size_t infilename_buckets;
	FILE **infiles;
	size_t infile_idx;
	size_t infile_buckets;
};
