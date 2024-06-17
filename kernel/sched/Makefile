# SPDX-License-Identifier: GPL-2.0
KBUILD_CFLAGS += -Wno-declaration-after-statement -Wno-invalid-noreturn
ifdef CONFIG_FUNCTION_TRACER
CFLAGS_REMOVE_clock.o = $(CC_FLAGS_FTRACE)
endif

# These files are disabled because they produce non-interesting flaky coverage
# that is not a function of syscall inputs. E.g. involuntary context switches.
KCOV_INSTRUMENT := n

# There are numerous data races here, however, most of them are due to plain accesses.
# This would make it even harder for syzbot to find reproducers, because these
# bugs trigger without specific input. Disable by default, but should re-enable
# eventually.
KCSAN_SANITIZE := n

CFLAGS_core.o := -fomit-frame-pointer

obj-y += core.o loadavg.o clock.o cputime.o
obj-y += idle.o fair.o rt.o deadline.o
obj-y += wait.o wait_bit.o swait.o completion.o
obj-y += fie_fixer.o

obj-$(CONFIG_SMP) += cpupri.o cpudeadline.o topology.o stop_task.o pelt.o
obj-$(CONFIG_SCHED_AUTOGROUP) += autogroup.o
obj-$(CONFIG_SCHEDSTATS) += stats.o
obj-$(CONFIG_SCHED_DEBUG) += debug.o
obj-$(CONFIG_CGROUP_CPUACCT) += cpuacct.o
obj-$(CONFIG_CPU_FREQ) += cpufreq.o
obj-$(CONFIG_CPU_FREQ_GOV_SCHEDUTIL) += cpufreq_schedutil.o
obj-$(CONFIG_CPU_FREQ_GOV_SCHEDHORIZON) += cpufreq_schedhorizon.o
obj-$(CONFIG_MEMBARRIER) += membarrier.o
obj-$(CONFIG_CPU_ISOLATION) += isolation.o
obj-$(CONFIG_PSI) += psi.o
obj-$(CONFIG_SCHED_WALT) += walt/
