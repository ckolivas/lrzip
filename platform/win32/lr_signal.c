/* Interrupt handling, Win32.                                     SESSION 8 */

#include "lr_platform.h"
#include "lr_win32.h"

bool lr_install_interrupt_handler(void (*handler)(void))
{
	(void)handler;
	return false;
}
