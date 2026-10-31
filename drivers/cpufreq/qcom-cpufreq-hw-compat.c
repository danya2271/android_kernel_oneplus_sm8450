// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compatibility module for vendor qcom_cpufreq_hw users.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/qcom-cpufreq-hw.h>

u64 qcom_cpufreq_get_cpu_cycle_counter(int cpu)
{
	return U64_MAX;
}
EXPORT_SYMBOL_GPL(qcom_cpufreq_get_cpu_cycle_counter);

static int __init qcom_cpufreq_hw_init(void)
{
	return 0;
}
module_init(qcom_cpufreq_hw_init);

MODULE_DESCRIPTION("QCOM CPUFreq HW compatibility module");
MODULE_LICENSE("GPL");
