/* Console echo control, POSIX.                                   SESSION 6 */

#include "lr_platform.h"
#include <stdio.h>

bool lr_echo_disable(void)
{
	struct termios t;
	int fd = fileno(stdin);

	if (tcgetattr(fd, &t))
		return false;
	t.c_lflag &= ~ECHO;
	return tcsetattr(fd, TCSANOW, &t) == 0;
}

bool lr_echo_enable(void)
{
	struct termios t;
	int fd = fileno(stdin);

	if (tcgetattr(fd, &t))
		return false;
	t.c_lflag |= ECHO;
	return tcsetattr(fd, TCSANOW, &t) == 0;
}
