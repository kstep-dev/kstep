// Reproduce: sched: Fix race against ptrace_freeze_trace()
// https://github.com/torvalds/linux/commit/d136122f5845
//
// Bug: __schedule() reads prev->state twice: once before rq->lock (READ 1)
// and once after (READ 2) as a double-check. ptrace_freeze_traced() can
// change task->state (TASK_TRACED -> __TASK_TRACED) between these reads,
// causing the double-check (prev_state == prev->state) to fail. The task
// is then not deactivated and stays on the runqueue with a non-zero state.
//
// Fix: Read prev->state only once, after rq->lock. Use a control dependency
// for ordering instead of the fragile double-check.
//
// Reproduce: A kprobe on update_rq_clock (called between READ 1 and READ 2)
// toggles the target task's state, simulating ptrace_freeze_traced(). On the
// buggy kernel the double-check fails and the task is never deactivated. On
// the fixed kernel the single-read logic correctly deactivates the task.

#include "driver.h"
#include "internal.h"
#include <linux/kprobes.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *target;
static volatile bool active;
static struct kprobe kp;

// Fires inside update_rq_clock, which is called between READ 1 and READ 2
// in __schedule() on the buggy kernel. Toggles the state so the double-check
// (prev_state == prev->state) always fails.
static int kp_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (!active || smp_processor_id() != 1)
		return 0;

	struct rq *rq = cpu_rq(1);
	if (rq->curr != target)
		return 0;

	long s = target->state;
	if (s == TASK_INTERRUPTIBLE)
		target->state = TASK_UNINTERRUPTIBLE;
	else if (s == TASK_UNINTERRUPTIBLE)
		target->state = TASK_INTERRUPTIBLE;
	return 0;
}

static void setup(void)
{
	target = kstep_kthread_create("target");
	kstep_kthread_bind(target, cpumask_of(1));
	kstep_kthread_start(target);

	kp.symbol_name = "update_rq_clock";
	kp.pre_handler = kp_pre;
	int ret = register_kprobe(&kp);
	if (ret < 0)
		pr_err("ptrace_freeze: register_kprobe failed: %d\n", ret);
	else
		TRACE_INFO("kprobe on update_rq_clock registered");
}

static void run(void)
{
	kstep_tick_repeat(5);

	// Block target: wait_event -> set_current_state(TASK_INTERRUPTIBLE) -> schedule()
	kstep_kthread_block(target);

	// Enable kprobe: toggle state between READ 1 and READ 2 in __schedule
	active = true;

	// Give time for target to enter wait_event and call schedule()
	kstep_tick_repeat(20);

	int on_rq = READ_ONCE(target->on_rq);
	long state = READ_ONCE(target->state);

	active = false;
	unregister_kprobe(&kp);

	TRACE_INFO("target: on_rq=%d state=0x%lx", on_rq, state);

	if (on_rq && state != 0) {
		kstep_fail("ptrace race: task stuck on_rq=%d state=0x%lx "
			   "(double-check prevented deactivation)", on_rq, state);
	} else {
		kstep_pass("task properly deactivated (on_rq=%d state=0x%lx)",
			   on_rq, state);
	}
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "ptrace_freeze",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
