// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility module for vendor OPLUS scheduler-assist users.
 */

#include <linux/init.h>
#include <linux/module.h>

static int __init oplus_sched_assist_init(void)
{
	return 0;
}
module_init(oplus_sched_assist_init);

MODULE_DESCRIPTION("OPLUS scheduler-assist compatibility module");
MODULE_LICENSE("GPL");
