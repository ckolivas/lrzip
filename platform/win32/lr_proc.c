/* Timing and process priority, Win32.                            SESSION 9 */

#include "lr_platform.h"
#include "lr_win32.h"

lr_i64 lr_time_ms(void)
{
	return -1;
}

bool lr_set_low_priority(void)
{
	return false;
}
