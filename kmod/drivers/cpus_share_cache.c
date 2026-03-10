// Reproduce: race cpus_share_cache()/update_top_cache_domain() (commit 42dc938a590c)
//
// Bug: cpus_share_cache(cpu, cpu) reads per_cpu(sd_llc_id, cpu) twice without
// synchronization. When update_top_cache_domain() modifies sd_llc_id between
// the two reads, the function returns false for same-CPU comparison. This
// causes ttwu_queue_cond() to incorrectly route a local wakeup through the
// wakelist, triggering WARN_ON_ONCE in ttwu_queue_wakelist().
//
// Fix: Add early return true when this_cpu == that_cpu.
//
// Strategy: A timer on CPU 1 calls cpus_share_cache(1,1) repeatedly. CPU 0
// sets sd_llc_id[1]=0 then triggers a domain rebuild (which sets it to 1).
// The buggy kernel has udelay(50) between the two reads, widening the race
// window. On the fixed kernel, cpus_share_cache(1,1) returns true immediately.

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 15, 0)

static struct hrtimer probe_timer;
static atomic_t timer_active;
static atomic_t race_count;

KSYM_IMPORT_TYPED(int, sd_llc_id);

static enum hrtimer_restart probe_timer_fn(struct hrtimer *timer)
{
	if (!atomic_read(&timer_active))
		return HRTIMER_NORESTART;

	// Call cpus_share_cache(1,1) from CPU 1 context.
	// On buggy kernel: reads sd_llc_id[1] twice with udelay(50) between.
	// If CPU 0 changes sd_llc_id[1] during the delay, returns false.
	// On fixed kernel: returns true immediately (early same-CPU check).
	typedef bool (*csc_fn_t)(int, int);
	csc_fn_t csc = (csc_fn_t)kstep_ksym_lookup("cpus_share_cache");
	bool result = csc(1, 1);
	if (!result)
		atomic_inc(&race_count);

	hrtimer_forward_now(timer, ns_to_ktime(1000));
	return HRTIMER_RESTART;
}

static void start_timer_on_cpu1(void *info)
{
	hrtimer_init(&probe_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	probe_timer.function = probe_timer_fn;
	atomic_set(&timer_active, 1);
	hrtimer_start(&probe_timer, ns_to_ktime(1000), HRTIMER_MODE_REL_PINNED);
}

static void stop_timer_on_cpu1(void *info)
{
	atomic_set(&timer_active, 0);
	hrtimer_cancel(&probe_timer);
}

static void setup(void)
{
	kstep_topo_init();
	atomic_set(&race_count, 0);
}

static void run(void)
{
	kstep_tick_repeat(5);

	TRACE_INFO("Initial sd_llc_id[1] = %d", *per_cpu_ptr(KSYM_sd_llc_id, 1));

	// Start probe timer on CPU 1 (fires every 1μs)
	smp_call_function_single(1, start_timer_on_cpu1, NULL, 1);
	TRACE_INFO("Probe timer started on CPU 1");

	// Rapidly rebuild sched domains from CPU 0.
	// Before each rebuild, reset sd_llc_id[1] to 0 so the rebuild
	// transition (0→1) creates a window where cpus_share_cache reads differ.
	for (int i = 0; i < 100; i++) {
		*per_cpu_ptr(KSYM_sd_llc_id, 1) = 0;
		// Allow time for timer to fire while sd_llc_id[1] is 0
		udelay(5);
		// Trigger domain rebuild: update_top_cache_domain sets sd_llc_id[1]=1
		kstep_topo_apply();
	}

	// Stop the timer
	smp_call_function_single(1, stop_timer_on_cpu1, NULL, 1);
	TRACE_INFO("Probe timer stopped");

	int races = atomic_read(&race_count);
	TRACE_INFO("Race detected %d times", races);
	TRACE_INFO("Final sd_llc_id[1] = %d", *per_cpu_ptr(KSYM_sd_llc_id, 1));

	if (races > 0) {
		kstep_fail("cpus_share_cache(1,1) returned false %d times", races);
	} else {
		kstep_pass("cpus_share_cache(1,1) always returned true");
	}
}

KSTEP_DRIVER_DEFINE{
    .name = "cpus_share_cache",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#endif
