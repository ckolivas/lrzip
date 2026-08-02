/* Console echo control, Win32.                                   SESSION 6 */

#include "lr_platform.h"
#include "lr_win32.h"

/* The console mode is a bitmask on the input handle rather than a struct of
   terminal settings, so both operations are the same read-modify-write with
   one bit flipped. ENABLE_ECHO_INPUT only has meaning while ENABLE_LINE_INPUT
   is set, which is the normal state for a cooked console -- exactly the case
   the passphrase prompt runs in. */
static bool lr_echo_set(bool on)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode;

	if (h == NULL || h == INVALID_HANDLE_VALUE)
		return false;

	/* Fails when stdin is a pipe or a file. There is no echo to suppress in
	   that case, and the POSIX side fails the same way on a non-tty. */
	if (!GetConsoleMode(h, &mode))
		return false;

	if (on)
		mode |= ENABLE_ECHO_INPUT;
	else
		mode &= ~ENABLE_ECHO_INPUT;

	return SetConsoleMode(h, mode) != 0;
}

bool lr_echo_disable(void)
{
	return lr_echo_set(false);
}

bool lr_echo_enable(void)
{
	return lr_echo_set(true);
}
