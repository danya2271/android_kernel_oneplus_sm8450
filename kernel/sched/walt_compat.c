// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility hooks for vendor modules that expect the WALT ABI.
 */

#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>

int sched_lpm_disallowed_time(int cpu, u64 *timeout)
{
	return INT_MAX;
}
EXPORT_SYMBOL(sched_lpm_disallowed_time);

int set_task_boost(int boost, u64 period)
{
	return 0;
}
EXPORT_SYMBOL(set_task_boost);

void core_ctl_notifier_register(struct notifier_block *n)
{
}
EXPORT_SYMBOL(core_ctl_notifier_register);

void core_ctl_notifier_unregister(struct notifier_block *n)
{
}
EXPORT_SYMBOL(core_ctl_notifier_unregister);

int core_ctl_set_boost(bool boost)
{
	return 0;
}
EXPORT_SYMBOL(core_ctl_set_boost);

int walt_pause_cpus(struct cpumask *cpus)
{
	return 0;
}
EXPORT_SYMBOL(walt_pause_cpus);

int walt_resume_cpus(struct cpumask *cpus)
{
	return 0;
}
EXPORT_SYMBOL(walt_resume_cpus);

static int __init walt_module_init(void)
{
	return 0;
}
module_init(walt_module_init);

MODULE_DESCRIPTION("Vendor WALT compatibility hooks");
MODULE_LICENSE("GPL");
