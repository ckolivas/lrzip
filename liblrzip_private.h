#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <lrzip_private.h>
#include <liblrzip.h>

#define INFILE_BUCKET_SIZE 10

struct Lrzip
{
	Lrzip_Mode mode;
	rzip_control *control;

	char *outfile;

	/* bucket allocation is used here to avoid frequent calls to realloc */
	char **infiles;
	size_t infile_idx;
	size_t infile_buckets;
};
