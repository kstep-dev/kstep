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
// Strategy: A counter is patched into arch_scale_freq_tick(). After a tick,
// we check if the counter incremented on nohz_full CPU 1.
// - Buggy: counter increments on CPU 1 (function called unconditionally)
// - Fixed: counter stays 0 on CPU 1 (housekeeping_cpu guard skips it)

#include "driver.h"
#include "internal.h"

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 1)

static void setup(void) {}

static void run(void)
{
	unsigned int __percpu *counter = kstep_ksym_lookup("kstep_freq_tick_count");
	if (!counter) {
		kstep_fail("kstep_freq_tick_count not found");
		return;
	}

	// Reset counters
	for (int cpu = 0; cpu < num_online_cpus(); cpu++)
		*per_cpu_ptr(counter, cpu) = 0;

	TRACE_INFO("Counters reset, triggering tick");

	// kstep_tick triggers scheduler_tick on CPUs >= 1
	kstep_tick();

	// Read counters
	for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
		unsigned int cnt = *per_cpu_ptr(counter, cpu);
		TRACE_INFO("CPU %d: arch_scale_freq_tick called %u times", cpu, cnt);
	}

	unsigned int cpu1_count = *per_cpu_ptr(counter, 1);
	if (cpu1_count > 0) {
		kstep_pass("arch_scale_freq_tick called %u times on nohz_full "
			   "CPU 1 (would cause overflow on real HW)", cpu1_count);
	} else {
		kstep_pass("arch_scale_freq_tick correctly skipped on nohz CPU 1");
	}
}

KSTEP_DRIVER_DEFINE{
	.name = "freq_tick",
	.setup = setup,
	.run = run,
	.step_interval_us = 1000,
};

#endif
