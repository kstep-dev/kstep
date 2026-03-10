// Reproduce: 7fb3ff22ad87 sched/core: Fix arch_scale_freq_tick() on tickless systems
//
// Bug: scheduler_tick() calls arch_scale_freq_tick() unconditionally on all
// CPUs. On nohz_full (tickless) CPUs, the APERF/MPERF delta accumulates over
// long periods without ticks, causing check_shl_overflow() to trigger in
// scale_freq_tick(), which disables frequency invariance globally with the
// warning "Scheduler frequency invariance went wobbly, disabling!"
//
// Fix: Guard arch_scale_freq_tick() with housekeeping_cpu(cpu, HK_TYPE_TICK)
// so it only runs on CPUs with regular ticks.
//
// Strategy: Poison the cpu_samples per-cpu data on a nohz_full CPU so the
// APERF delta underflows to a huge value, then trigger scheduler_tick().

#include "driver.h"
#include "internal.h"

#include <linux/version.h>
#include <asm/msr.h>
#include <asm/cpufeature.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 1)

// Mirror of struct aperfmperf from arch/x86/kernel/cpu/aperfmperf.c
struct aperfmperf_kstep {
	seqcount_t	seq;
	unsigned long	last_update;
	u64		acnt;
	u64		mcnt;
	u64		aperf;
	u64		mperf;
};

struct poison_args {
	void __percpu *samples;
};

// Runs on target CPU to poison its cpu_samples
static void poison_cpu_samples(void *data)
{
	struct poison_args *args = data;
	struct aperfmperf_kstep *s = this_cpu_ptr(args->samples);
	int cpu = smp_processor_id();

	if (!boot_cpu_has(X86_FEATURE_APERFMPERF)) {
		TRACE_INFO("CPU %d: APERFMPERF not available, poisoning anyway", cpu);
		// Even without real MSRs, set stored aperf high.
		// arch_scale_freq_tick() will return early if feature not available.
		s->aperf = (1ULL << 62);
		s->mperf = (1ULL << 62);
		return;
	}

	u64 aperf, mperf;
	rdmsrl(MSR_IA32_APERF, aperf);
	rdmsrl(MSR_IA32_MPERF, mperf);

	TRACE_INFO("CPU %d: real APERF=%llu MPERF=%llu stored aperf=%llu mperf=%llu",
		   cpu, aperf, mperf, s->aperf, s->mperf);

	// Set stored value much larger than real → unsigned underflow on next read.
	// Delta will be ~(2^64 - 2^60) which overflows check_shl_overflow(_, 20, _).
	s->aperf = aperf + (1ULL << 60);
	s->mperf = mperf + (1ULL << 60);

	TRACE_INFO("CPU %d: poisoned aperf=%llu mperf=%llu", cpu, s->aperf, s->mperf);
}

static void setup(void) {}

static void run(void)
{
	// Look up cpu_samples per-cpu variable
	void __percpu *cpu_samples = kstep_ksym_lookup("cpu_samples");
	if (!cpu_samples) {
		kstep_fail("cpu_samples symbol not found");
		return;
	}

	// Look up arch_scale_freq_key to check/enable freq invariance
	struct static_key_false *freq_key = kstep_ksym_lookup("arch_scale_freq_key");
	if (!freq_key) {
		TRACE_INFO("arch_scale_freq_key not found (freq invariance compiled out?)");
	} else {
		bool enabled = static_key_enabled(&freq_key->key);
		TRACE_INFO("arch_scale_freq_key enabled=%d", enabled);
		if (!enabled) {
			TRACE_INFO("Force-enabling arch_scale_freq_key");
			static_branch_enable(freq_key);
		}
	}

	// Check APERFMPERF availability
	TRACE_INFO("APERFMPERF available: %d", boot_cpu_has(X86_FEATURE_APERFMPERF));

	// Poison cpu_samples on CPU 1 (nohz_full)
	struct poison_args args = { .samples = cpu_samples };
	smp_call_function_single(1, poison_cpu_samples, &args, 1);

	// Trigger scheduler_tick on all CPUs ≥ 1.
	// Buggy kernel: unconditionally calls arch_scale_freq_tick() → overflow
	// Fixed kernel: housekeeping_cpu(1, HK_TYPE_TICK)=false → skips it
	kstep_tick();

	// Try to flush the disable work if it was scheduled
	struct work_struct *disable_work = kstep_ksym_lookup("disable_freq_invariance_work");
	if (disable_work)
		flush_work(disable_work);

	// Check if freq invariance was disabled (= bug triggered)
	if (freq_key) {
		bool still_enabled = static_key_enabled(&freq_key->key);
		TRACE_INFO("After tick: arch_scale_freq_key enabled=%d", still_enabled);
		if (!still_enabled) {
			kstep_pass("freq invariance went wobbly (overflow triggered)");
		} else {
			kstep_fail("freq invariance still enabled (overflow did not trigger)");
		}
	} else {
		kstep_fail("cannot check freq_key state");
	}
}

KSTEP_DRIVER_DEFINE{
	.name = "freq_tick",
	.setup = setup,
	.run = run,
	.step_interval_us = 1000,
};

#endif
