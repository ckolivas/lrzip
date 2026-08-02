/* Interrupt handling, POSIX.                                     SESSION 8 */

#include "lr_platform.h"

bool lr_install_interrupt_handler(void (*handler)(void))
{
	(void)handler;
	return false;
}
