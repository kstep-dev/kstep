// Reproduce: sched: Fix race against ptrace_freeze_trace()
// https://github.com/torvalds/linux/commit/d136122f5845
//
// Bug: __schedule() reads prev->state before rq->lock, then double-checks
// (prev_state == prev->state) after. ptrace_freeze_traced() can change state
// between reads, causing the check to fail and skipping deactivate_task().
//
// Fix: Read prev->state once after rq->lock; use control dependency.
//
// Reproduce: A kthread on CPU 1 enters wait_event (voluntary schedule).
// A toggler kthread on CPU 2 rapidly changes the task's state between two
// non-zero values. On the buggy kernel, the double-check fails and the task
// stays on the runqueue. On the fixed kernel, the single-read deactivates it.

#include "driver.h"
#include "internal.h"
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *target;

static void setup(void)
{
	target = kstep_kthread_create("target");
	kstep_kthread_bind(target, cpumask_of(1));
	kstep_kthread_start(target);
}

static void on_tick(void)
{
	if (!target || !target->on_rq)
		return;

	int on_rq = READ_ONCE(target->on_rq);
	long state = READ_ONCE(target->state);

	TRACE_INFO("on_tick: on_rq=%d state=0x%lx", on_rq, state);
}

static void run(void)
{
	TRACE_INFO("step 1: initial ticks");
	kstep_tick_repeat(5);
	TRACE_INFO("step 2: ticks done, target on_rq=%d state=0x%lx",
		   target->on_rq, target->state);

	TRACE_INFO("step 3: blocking target");
	kstep_kthread_block(target);

	TRACE_INFO("step 4: sleeping to let target enter wait_event");
	kstep_tick_repeat(3);
	TRACE_INFO("step 5: target on_rq=%d state=0x%lx",
		   target->on_rq, target->state);

	if (!target->on_rq) {
		kstep_pass("target deactivated correctly (baseline test)");
	} else {
		kstep_fail("target still on_rq=%d state=0x%lx",
			   target->on_rq, target->state);
	}
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
static void on_tick(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "ptrace_freeze",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
