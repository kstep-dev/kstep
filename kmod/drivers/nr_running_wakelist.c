// Reproduce: incorrect nr_running check in ttwu_queue_cond (commit 28156108fecb)
//
// Bug: ttwu_queue_cond() uses `nr_running <= 1` instead of `!nr_running`
// when deciding whether to offload a wakeup to the wakelist with WF_ON_CPU.
// Since the descheduling task has already been deactivated (accounted out of
// nr_running), the only-runnable-task case means nr_running == 0, not <= 1.
// The buggy check causes unnecessary wakelist queueing when nr_running == 1
// (i.e., there IS another runnable task), leading to suboptimal scheduling.
//
// Fix: Change condition from `nr_running <= 1` to `!nr_running`.

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 19, 0)

static struct task_struct *extra_task;
static struct task_struct *wakee;

// IPI handler: runs on CPU 1 to clear on_cpu after a brief delay.
// On the fixed kernel, try_to_wake_up falls through to
// smp_cond_load_acquire(&p->on_cpu, !VAL) which spins until on_cpu=0.
// This IPI ensures it eventually returns.
static void delayed_clear_on_cpu(void *info)
{
	struct task_struct *p = info;
	udelay(100);
	smp_store_release(&p->on_cpu, 0);
}

static void setup(void)
{
	extra_task = kstep_task_create();
	kstep_task_pin(extra_task, 1, 1);

	wakee = kstep_kthread_create("wakee");
	kstep_kthread_bind(wakee, cpumask_of(1));
	kstep_kthread_start(wakee);
}

static void run(void)
{
	unsigned int nr;
	struct rq *rq1 = cpu_rq(1);

	kstep_tick_repeat(5);

	// Block the wakee so it sleeps on CPU 1's waitqueue
	kstep_kthread_block(wakee);
	kstep_sleep();
	kstep_sleep();

	// CPU 1 should have: extra_task running, wakee sleeping
	nr = rq1->nr_running;
	TRACE_INFO("CPU 1 nr_running = %u (expect 1)", nr);

	if (nr != 1) {
		TRACE_INFO("Unexpected nr_running=%u, skipping test", nr);
		return;
	}

	// Verify CPUs share cache (needed for the buggy condition to matter)
	typedef bool (*csc_fn_t)(int, int);
	csc_fn_t csc = (csc_fn_t)kstep_ksym_lookup("cpus_share_cache");
	bool shared = csc(0, 1);
	TRACE_INFO("cpus_share_cache(0,1) = %d", shared);
	if (!shared) {
		TRACE_INFO("CPUs 0 and 1 don't share cache, skipping");
		return;
	}

	// Simulate the descheduling window: set on_cpu=1 so try_to_wake_up
	// sets WF_ON_CPU. Queue an IPI on CPU 1 to clear on_cpu after 100us,
	// ensuring try_to_wake_up's smp_cond_load_acquire doesn't hang.
	WRITE_ONCE(wakee->on_cpu, 1);
	smp_call_function_single(1, delayed_clear_on_cpu, wakee, 0);

	// Wake from CPU 0 — try_to_wake_up sees on_cpu=1, sets WF_ON_CPU.
	// Buggy kernel: ttwu_queue_wakelist returns true → wakelist path,
	//   returns quickly. The task activation waits for the IPI chain.
	// Fixed kernel: ttwu_queue_wakelist returns false → spins on
	//   smp_cond_load_acquire until the IPI clears on_cpu (~100us),
	//   then does direct activation synchronously.
	wake_up_process(wakee);

	// Read nr_running right after wake_up_process returns.
	unsigned int nr_after = rq1->nr_running;

	TRACE_INFO("nr_running before=%u, after=%u", nr, nr_after);

	if (nr_after == nr) {
		kstep_fail("wakelist used with nr_running=%u (should only when 0)", nr);
	} else {
		kstep_pass("direct activation: nr_running %u -> %u", nr, nr_after);
	}

	kstep_sleep();
	kstep_tick_repeat(5);
}

KSTEP_DRIVER_DEFINE{
    .name = "nr_running_wakelist",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#endif
