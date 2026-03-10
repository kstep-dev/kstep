// Reproduce: 7fb3ff22ad87 sched/core: Fix arch_scale_freq_tick() on tickless systems
//
// Bug: scheduler_tick() calls arch_scale_freq_tick() unconditionally on all
// CPUs. On nohz_full CPUs, large APERF/MPERF deltas cause overflow in
// scale_freq_tick()'s check_shl_overflow(), disabling frequency invariance
// globally ("Scheduler frequency invariance went wobbly").
//
// Fix: Guard with housekeeping_cpu(cpu, HK_TYPE_TICK) so it only runs on
// CPUs with regular ticks.
//
// Strategy: Use a kprobe on arch_scale_freq_tick to count calls per CPU.
// After a scheduler tick, check whether the function was called on the
// nohz_full CPU 1 (which is non-housekeeping due to isolcpus=nohz,...,1).
// - Buggy: kprobe fires on CPU 1 (function called unconditionally)
// - Fixed: kprobe does NOT fire on CPU 1 (housekeeping_cpu guard skips it)

#include "driver.h"
#include "internal.h"

#include <linux/kprobes.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 1)

static DEFINE_PER_CPU(unsigned int, freq_tick_count);

static int freq_tick_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	this_cpu_inc(freq_tick_count);
	return 0;
}

static struct kprobe freq_tick_kp = {
	.symbol_name = "arch_scale_freq_tick",
	.pre_handler = freq_tick_pre_handler,
};

static void setup(void) {}

static void run(void)
{
	int ret = register_kprobe(&freq_tick_kp);
	if (ret < 0) {
		kstep_fail("Failed to register kprobe on arch_scale_freq_tick: %d", ret);
		return;
	}

	for (int cpu = 0; cpu < num_online_cpus(); cpu++)
		per_cpu(freq_tick_count, cpu) = 0;

	TRACE_INFO("Kprobe registered, triggering tick");

	// kstep_tick triggers scheduler_tick on CPUs >= 1
	kstep_tick();

	// Disable kprobe before reading results to avoid re-entry
	disable_kprobe(&freq_tick_kp);

	for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
		unsigned int cnt = per_cpu(freq_tick_count, cpu);
		TRACE_INFO("CPU %d: arch_scale_freq_tick called %u times", cpu, cnt);
	}

	unsigned int cpu1_count = per_cpu(freq_tick_count, 1);

	if (cpu1_count > 0) {
		kstep_pass("Bug: arch_scale_freq_tick called %u times on "
			   "non-housekeeping CPU 1", cpu1_count);
	} else {
		kstep_fail("arch_scale_freq_tick not called on CPU 1");
	}
}

KSTEP_DRIVER_DEFINE{
	.name = "freq_tick",
	.setup = setup,
	.run = run,
	.step_interval_us = 1000,
};

#endif
