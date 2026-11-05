// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu.h>
#include <linux/cpu_input_boost.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/kernel_stat.h>
#include <linux/kknx_perf.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/msm_kgsl.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>
#include <soc/qcom/dcvs.h>

#define KKNX_SAMPLE_PERIOD		HZ
#define KKNX_WINDOW_SAMPLES		5
#define KKNX_CPU_MINIMAL_X100		2000
#define KKNX_CPU_HIGH_X100		5000
#define KKNX_GPU_HIGH_X100		3000

struct kknx_window {
	u64 busy[KKNX_WINDOW_SAMPLES];
	u64 total[KKNX_WINDOW_SAMPLES];
	u64 busy_sum;
	u64 total_sum;
	u32 valid_count;
	u32 next;
};

struct kknx_gpu_window {
	u32 percent[KKNX_WINDOW_SAMPLES];
	u32 valid[KKNX_WINDOW_SAMPLES];
	u32 percent_sum;
	u32 count;
	u32 valid_count;
	u32 next;
};

struct kknx_cpu_floor {
	struct cpufreq_policy *policy;
	struct freq_qos_request request;
	s32 floor_khz;
};

struct kknx_perf_state {
	struct delayed_work sample_work;
	struct kernel_cpustat previous[NR_CPUS];
	cpumask_t previous_valid;
	struct kknx_window cpu;
	struct kknx_gpu_window gpu;
	struct kknx_cpu_floor cpu_floors[NR_CPUS];
	bool cpu_floors_active;
	enum kknx_perf_load_level level;
	bool probe_60hz;
	bool sample_valid;
	u32 display_hz;
	bool display_hz_valid;
	unsigned long gpu_floor_hz;
	unsigned long dcvs_override_mask;
};

static struct kknx_perf_state kknx_perf;
static DEFINE_MUTEX(kknx_display_ops_lock);
static const struct kknx_perf_display_ops *kknx_display_ops;

#if IS_ENABLED(CONFIG_QCOM_DCVS)
static int kknx_dcvs_set_min_override(enum dcvs_hw_type hw_type, bool enable)
{
	int (*set_override)(enum dcvs_hw_type, bool);
	int ret;

	set_override = symbol_get(qcom_dcvs_hw_min_override_set);
	if (!set_override)
		return -ENODEV;
	ret = set_override(hw_type, enable);
	symbol_put(qcom_dcvs_hw_min_override_set);
	return ret;
}

static void kknx_dcvs_apply(bool minimal)
{
	static const enum dcvs_hw_type hw_types[] = {
		DCVS_DDR, DCVS_LLCC, DCVS_L3,
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(hw_types); i++) {
		enum dcvs_hw_type hw_type = hw_types[i];
		unsigned long mask = BIT(hw_type);

		if (minimal && !(kknx_perf.dcvs_override_mask & mask)) {
			ret = kknx_dcvs_set_min_override(hw_type, true);
			if (!ret)
				kknx_perf.dcvs_override_mask |= mask;
		} else if (!minimal &&
			   (kknx_perf.dcvs_override_mask & mask)) {
			ret = kknx_dcvs_set_min_override(hw_type, false);
			if (!ret)
				kknx_perf.dcvs_override_mask &= ~mask;
		}
	}
}
#else
static void kknx_dcvs_apply(bool minimal)
{
}
#endif

int kknx_perf_register_display_ops(const struct kknx_perf_display_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->get_refresh_rate || !ops->update_policy)
		return -EINVAL;

	mutex_lock(&kknx_display_ops_lock);
	if (kknx_display_ops)
		ret = -EBUSY;
	else
		kknx_display_ops = ops;
	mutex_unlock(&kknx_display_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kknx_perf_register_display_ops);

void kknx_perf_unregister_display_ops(const struct kknx_perf_display_ops *ops)
{
	mutex_lock(&kknx_display_ops_lock);
	if (kknx_display_ops == ops) {
		kknx_display_ops->update_policy(KKNX_PERF_LEVEL_UNKNOWN, false);
		WRITE_ONCE(kknx_perf.sample_valid, false);
		kknx_display_ops = NULL;
	}
	mutex_unlock(&kknx_display_ops_lock);
}
EXPORT_SYMBOL_GPL(kknx_perf_unregister_display_ops);

static int kknx_get_display_refresh_rate(u32 *refresh_rate)
{
	int ret = -ENODEV;

	mutex_lock(&kknx_display_ops_lock);
	if (kknx_display_ops)
		ret = kknx_display_ops->get_refresh_rate(refresh_rate);
	mutex_unlock(&kknx_display_ops_lock);
	return ret;
}

static void kknx_update_display_policy(u32 load_level, bool probe_60hz)
{
	mutex_lock(&kknx_display_ops_lock);
	if (kknx_display_ops)
		kknx_display_ops->update_policy(load_level, probe_60hz);
	mutex_unlock(&kknx_display_ops_lock);
}

static int kknx_get_gpu_stats(u32 *busy_percent, unsigned long *freq_hz)
{
	int (*get_stats)(u32 *, unsigned long *);
	int ret;

	get_stats = symbol_get(kgsl_kknx_get_gpu_stats);
	if (!get_stats)
		return -ENODEV;
	ret = get_stats(busy_percent, freq_hz);
	symbol_put(kgsl_kknx_get_gpu_stats);
	return ret;
}

static int kknx_set_gpu_min_freq(unsigned long freq_hz)
{
	int (*set_min_freq)(unsigned long);
	int ret;

	set_min_freq = symbol_get(kgsl_kknx_set_gpu_min_freq);
	if (!set_min_freq)
		return -ENODEV;
	ret = set_min_freq(freq_hz);
	symbol_put(kgsl_kknx_set_gpu_min_freq);
	return ret;
}

static const char *kknx_perf_level_name(enum kknx_perf_load_level level)
{
	switch (level) {
	case KKNX_PERF_LEVEL_MINIMAL:
		return "minimal";
	case KKNX_PERF_LEVEL_MEDIUM:
		return "medium";
	case KKNX_PERF_LEVEL_HIGH:
		return "high";
	default:
		return "unknown";
	}
}

static void kknx_window_add(struct kknx_window *window, u64 busy, u64 total)
{
	u32 slot = window->next;

	if (window->valid_count == KKNX_WINDOW_SAMPLES) {
		window->busy_sum -= window->busy[slot];
		window->total_sum -= window->total[slot];
	} else {
		window->valid_count++;
	}

	window->busy[slot] = busy;
	window->total[slot] = total;
	window->busy_sum += busy;
	window->total_sum += total;
	window->next = (slot + 1) % KKNX_WINDOW_SAMPLES;
}

static u32 kknx_window_cpu_percent_x100(const struct kknx_window *window)
{
	if (window->valid_count != KKNX_WINDOW_SAMPLES || !window->total_sum)
		return U32_MAX;

	return div64_u64(window->busy_sum * 10000, window->total_sum);
}

static void kknx_gpu_window_add(struct kknx_gpu_window *window, u32 percent,
				bool valid)
{
	u32 slot = window->next;

	if (window->count == KKNX_WINDOW_SAMPLES) {
		window->percent_sum -= window->percent[slot];
		window->valid_count -= window->valid[slot];
	} else
		window->count++;

	window->percent[slot] = valid ? min(percent, 100U) : 0;
	window->valid[slot] = valid;
	window->percent_sum += window->percent[slot];
	window->valid_count += window->valid[slot];
	window->next = (slot + 1) % KKNX_WINDOW_SAMPLES;
}

static u32 kknx_window_gpu_percent_x100(const struct kknx_gpu_window *window)
{
	if (window->valid_count != KKNX_WINDOW_SAMPLES)
		return 0;

	return div_u64((u64)window->percent_sum * 100, window->valid_count);
}

static bool kknx_load_is_minimal(u32 cpu_percent_x100, u32 display_hz)
{
	return cpu_percent_x100 <= KKNX_CPU_MINIMAL_X100 && display_hz <= 60;
}

static bool kknx_load_is_medium(u32 cpu_percent_x100)
{
	return cpu_percent_x100 > KKNX_CPU_MINIMAL_X100;
}

static bool kknx_load_is_high(u32 cpu_percent_x100, u32 display_hz,
			      u32 gpu_percent_x100, bool gpu_valid)
{
	return gpu_valid && cpu_percent_x100 > KKNX_CPU_HIGH_X100 &&
	       display_hz == 120 && gpu_percent_x100 > KKNX_GPU_HIGH_X100;
}

static enum kknx_perf_load_level
kknx_classify_load(u32 cpu_percent_x100, u32 display_hz,
		   u32 gpu_percent_x100, bool gpu_valid)
{
	if (kknx_load_is_high(cpu_percent_x100, display_hz,
			      gpu_percent_x100, gpu_valid))
		return KKNX_PERF_LEVEL_HIGH;
	if (kknx_load_is_minimal(cpu_percent_x100, display_hz))
		return KKNX_PERF_LEVEL_MINIMAL;
	if (kknx_load_is_medium(cpu_percent_x100))
		return KKNX_PERF_LEVEL_MEDIUM;
	return KKNX_PERF_LEVEL_UNKNOWN;
}

static bool kknx_should_probe_60hz(enum kknx_perf_load_level level,
				   u32 cpu_percent_x100, u32 display_hz)
{
	return level == KKNX_PERF_LEVEL_UNKNOWN &&
	       cpu_percent_x100 <= KKNX_CPU_MINIMAL_X100 &&
	       display_hz == 120;
}

enum kknx_perf_load_level kknx_perf_get_load_level(void)
{
	return READ_ONCE(kknx_perf.level);
}
EXPORT_SYMBOL_GPL(kknx_perf_get_load_level);

bool kknx_perf_is_minimal_load(void)
{
	return kknx_perf_get_load_level() == KKNX_PERF_LEVEL_MINIMAL;
}
EXPORT_SYMBOL_GPL(kknx_perf_is_minimal_load);

bool kknx_perf_is_medium_load(void)
{
	return kknx_perf_get_load_level() == KKNX_PERF_LEVEL_MEDIUM;
}
EXPORT_SYMBOL_GPL(kknx_perf_is_medium_load);

bool kknx_perf_is_high_load(void)
{
	return kknx_perf_get_load_level() == KKNX_PERF_LEVEL_HIGH;
}
EXPORT_SYMBOL_GPL(kknx_perf_is_high_load);

bool kknx_perf_use_minimal_cpu_floor(void)
{
	return READ_ONCE(kknx_perf.sample_valid) &&
		kknx_perf_get_load_level() == KKNX_PERF_LEVEL_MINIMAL;
}
EXPORT_SYMBOL_GPL(kknx_perf_use_minimal_cpu_floor);

static bool kknx_sample_cpu(u64 *busy_delta, u64 *total_delta)
{
	struct kernel_cpustat snapshot;
	u64 busy, total;
	int cpu, field;

	*busy_delta = 0;
	*total_delta = 0;
	for_each_possible_cpu(cpu) {
		if (!cpu_online(cpu)) {
			cpumask_clear_cpu(cpu, &kknx_perf.previous_valid);
			continue;
		}

		kcpustat_cpu_fetch(&snapshot, cpu);
		if (cpumask_test_cpu(cpu, &kknx_perf.previous_valid)) {
			busy = 0;
			total = 0;
			for (field = CPUTIME_USER; field <= CPUTIME_STEAL; field++) {
				u64 now = snapshot.cpustat[field];
				u64 old = kknx_perf.previous[cpu].cpustat[field];
				u64 delta = now >= old ? now - old : 0;

				total += delta;
				if (field != CPUTIME_IDLE && field != CPUTIME_IOWAIT)
					busy += delta;
			}
			*busy_delta += busy;
			*total_delta += total;
		}

		kknx_perf.previous[cpu] = snapshot;
		cpumask_set_cpu(cpu, &kknx_perf.previous_valid);
	}

	return *total_delta != 0;
}

static struct kknx_cpu_floor *kknx_find_cpu_floor(
		struct cpufreq_policy *policy)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kknx_perf.cpu_floors); i++)
		if (kknx_perf.cpu_floors[i].policy == policy)
			return &kknx_perf.cpu_floors[i];
	return NULL;
}

static struct kknx_cpu_floor *kknx_find_free_cpu_floor(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kknx_perf.cpu_floors); i++)
		if (!kknx_perf.cpu_floors[i].policy)
			return &kknx_perf.cpu_floors[i];
	return NULL;
}

static void kknx_cpu_floors_release(void)
{
	struct kknx_cpu_floor *floor;
	int i;

	if (!kknx_perf.cpu_floors_active)
		return;

	for (i = 0; i < ARRAY_SIZE(kknx_perf.cpu_floors); i++) {
		floor = &kknx_perf.cpu_floors[i];
		if (!floor->policy)
			continue;
		freq_qos_remove_request(&floor->request);
		cpufreq_cpu_put(floor->policy);
		floor->policy = NULL;
		floor->floor_khz = 0;
	}
	kknx_perf.cpu_floors_active = false;
}

static void kknx_cpu_floors_update(enum kknx_perf_load_level level)
{
	struct kknx_cpu_floor *floor;
	struct cpufreq_policy *policy;
	unsigned int cpu, leader;
	unsigned int target_khz;
	int i, ret;

	if (level == KKNX_PERF_LEVEL_UNKNOWN)
		return;

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		leader = cpumask_first_and(policy->related_cpus, cpu_online_mask);
		if (leader != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}

		floor = kknx_find_cpu_floor(policy);
		if (floor) {
			if (level == KKNX_PERF_LEVEL_MINIMAL)
				target_khz = policy->cpuinfo.min_freq;
			else
				target_khz = max(policy->cur,
						 (unsigned int)floor->floor_khz);
			if (target_khz != floor->floor_khz) {
				ret = freq_qos_update_request(&floor->request,
							      target_khz);
				if (!ret)
					floor->floor_khz = target_khz;
			}
			cpufreq_cpu_put(policy);
			continue;
		}

		floor = kknx_find_free_cpu_floor();
		if (!floor) {
			cpufreq_cpu_put(policy);
			break;
		}
		target_khz = level == KKNX_PERF_LEVEL_MINIMAL ?
			policy->cpuinfo.min_freq : policy->cur;
		ret = freq_qos_add_request(&policy->constraints,
					   &floor->request, FREQ_QOS_MIN,
					   target_khz);
		if (ret) {
			cpufreq_cpu_put(policy);
			continue;
		}
		floor->policy = policy;
		floor->floor_khz = target_khz;
		kknx_perf.cpu_floors_active = true;
	}

	/* Drop references for policies removed while the floor was active. */
	for (i = 0; i < ARRAY_SIZE(kknx_perf.cpu_floors); i++) {
		floor = &kknx_perf.cpu_floors[i];
		if (floor->policy &&
		    !cpumask_intersects(floor->policy->related_cpus,
					cpu_online_mask)) {
			freq_qos_remove_request(&floor->request);
			cpufreq_cpu_put(floor->policy);
			floor->policy = NULL;
			floor->floor_khz = 0;
		}
	}
}

static void kknx_apply_level(enum kknx_perf_load_level level,
			     unsigned long gpu_freq_hz, bool probe_60hz)
{
	enum kknx_perf_load_level old = READ_ONCE(kknx_perf.level);
	bool old_probe = kknx_perf.probe_60hz;
	int ret;

	if (old == level && old_probe == probe_60hz)
		return;

	WRITE_ONCE(kknx_perf.level, level);
	WRITE_ONCE(kknx_perf.probe_60hz, probe_60hz);
	cpu_input_boost_update_policy();
	if (level == KKNX_PERF_LEVEL_MINIMAL) {
		if (kknx_perf.gpu_floor_hz) {
			kknx_set_gpu_min_freq(0);
			kknx_perf.gpu_floor_hz = 0;
		}
	} else if (level == KKNX_PERF_LEVEL_MEDIUM ||
		   level == KKNX_PERF_LEVEL_HIGH) {
		if (level != KKNX_PERF_LEVEL_HIGH && kknx_perf.gpu_floor_hz) {
			kknx_set_gpu_min_freq(0);
			kknx_perf.gpu_floor_hz = 0;
		}
		if (level == KKNX_PERF_LEVEL_HIGH && gpu_freq_hz >
		    kknx_perf.gpu_floor_hz) {
			ret = kknx_set_gpu_min_freq(gpu_freq_hz);
			if (!ret)
				kknx_perf.gpu_floor_hz = gpu_freq_hz;
		}
	} else if (old == KKNX_PERF_LEVEL_HIGH && kknx_perf.gpu_floor_hz) {
		kknx_set_gpu_min_freq(0);
		kknx_perf.gpu_floor_hz = 0;
	}

	kknx_update_display_policy(level, probe_60hz);
	pr_info_ratelimited("kknx_perf: %s -> %s\n",
			    kknx_perf_level_name(old),
			    kknx_perf_level_name(level));
}

static void kknx_perf_sample_work(struct work_struct *work)
{
	u64 cpu_busy, cpu_total;
	u32 gpu_busy = 0, display_hz = 0;
	u32 cpu_percent_x100, gpu_percent_x100;
	unsigned long gpu_freq = 0;
	bool gpu_valid, probe_60hz = false;
	bool valid_sample = false;
	bool refresh_changed = false;
	bool policy_changed;
	int rc;
	enum kknx_perf_load_level level = KKNX_PERF_LEVEL_UNKNOWN;

	if (kknx_sample_cpu(&cpu_busy, &cpu_total))
		kknx_window_add(&kknx_perf.cpu, cpu_busy, cpu_total);

	rc = kknx_get_gpu_stats(&gpu_busy, &gpu_freq);
	gpu_valid = !rc;
	kknx_gpu_window_add(&kknx_perf.gpu, gpu_busy, gpu_valid);
	rc = kknx_get_display_refresh_rate(&display_hz);

	cpu_percent_x100 =
		kknx_window_cpu_percent_x100(&kknx_perf.cpu);
	gpu_percent_x100 =
		kknx_window_gpu_percent_x100(&kknx_perf.gpu);
	if ((!rc || rc == -EAGAIN) && cpu_percent_x100 != U32_MAX) {
		valid_sample = display_hz != 0;
		level = kknx_classify_load(cpu_percent_x100, display_hz,
					   gpu_percent_x100,
					   kknx_perf.gpu.valid_count ==
						KKNX_WINDOW_SAMPLES);
		probe_60hz = kknx_should_probe_60hz(level, cpu_percent_x100,
						    display_hz);
	}

	if (valid_sample) {
		refresh_changed = !READ_ONCE(kknx_perf.display_hz_valid) ||
			(READ_ONCE(kknx_perf.display_hz) > 60) !=
			(display_hz > 60);
		policy_changed = level != READ_ONCE(kknx_perf.level) ||
			probe_60hz != READ_ONCE(kknx_perf.probe_60hz);
		WRITE_ONCE(kknx_perf.display_hz, display_hz);
		WRITE_ONCE(kknx_perf.display_hz_valid, true);
		WRITE_ONCE(kknx_perf.sample_valid, true);
		kknx_apply_level(level, gpu_freq, probe_60hz);
		if (refresh_changed && !policy_changed)
			cpu_input_boost_update_policy();
		kknx_cpu_floors_update(level);
		kknx_dcvs_apply(level == KKNX_PERF_LEVEL_MINIMAL);
		if (level == KKNX_PERF_LEVEL_HIGH && gpu_freq >
		    kknx_perf.gpu_floor_hz) {
			if (!kknx_set_gpu_min_freq(gpu_freq))
				kknx_perf.gpu_floor_hz = gpu_freq;
		}
	}

	queue_delayed_work(system_freezable_wq, &kknx_perf.sample_work,
			   KKNX_SAMPLE_PERIOD);
}

static int __init kknx_perf_init(void)
{
	u64 busy, total;

	INIT_DELAYED_WORK(&kknx_perf.sample_work, kknx_perf_sample_work);
	kknx_sample_cpu(&busy, &total);
	queue_delayed_work(system_freezable_wq, &kknx_perf.sample_work,
			   KKNX_SAMPLE_PERIOD);
	return 0;
}
module_init(kknx_perf_init);

static void __exit kknx_perf_exit(void)
{
	cancel_delayed_work_sync(&kknx_perf.sample_work);
	kknx_cpu_floors_release();
	kknx_set_gpu_min_freq(0);
	kknx_dcvs_apply(false);
	kknx_update_display_policy(KKNX_PERF_LEVEL_UNKNOWN, false);
}
module_exit(kknx_perf_exit);

MODULE_DESCRIPTION("KKNX five-second load-aware performance policy");
MODULE_LICENSE("GPL");
