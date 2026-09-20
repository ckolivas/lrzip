/* Empty temporary-directory variables must not override later choices. */
#include "lrzip_private.h"
#include "lrzip_core.h"

int main(void)
{
	const char *names[] = { "TMPDIR", "TMP", "TEMPDIR", "TEMP" };
	const char *paths[] = { "tmpdir", "tmp", "tempdir", "temp" };
	unsigned mask, i;

	/* Each variable is unset, empty, a path, or a path with a slash. */
	for (mask = 0; mask < 256; mask++) {
		rzip_control control;
		char expected[32] = "./";
		bool selected = false;

		for (i = 0; i < 4; i++) {
			unsigned state = (mask >> (2 * i)) & 3;
			char value[32];
			int ret;

			if (!state) {
				ret = unsetenv(names[i]);
			} else {
				if (state == 1)
					value[0] = '\0';
				else
					snprintf(value, sizeof(value), "%s%s", paths[i],
						 state == 3 ? "/" : "");
				ret = setenv(names[i], value, 1);
			}
			if (ret) {
				perror(names[i]);
				return 1;
			}
			if (!selected && state >= 2) {
				snprintf(expected, sizeof(expected), "%s/", paths[i]);
				selected = true;
			}
		}
		if (!initialise_control(&control))
			return 1;
		if (strcmp(control.tmpdir, expected)) {
			fprintf(stderr, "Temporary-directory case %u: expected '%s', got '%s'\n",
				mask, expected, control.tmpdir);
			return 1;
		}
		free(control.tmpdir);
		if (pthread_mutex_destroy(&control.control_lock))
			return 1;
	}
	puts("Temporary-directory environment tests passed (256 combinations)");
	return 0;
}
